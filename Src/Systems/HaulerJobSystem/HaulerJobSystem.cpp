#include "Systems/HaulerJobSystem/HaulerJobSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/Geometry/Geometry.h"
#include "Utils/MoneyFormat/MoneyFormat.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <string>
#include <utility>
#include <vector>

namespace
{
// Игровые цвета сообщений (как в PortJobSystem/BusJobSystem).
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Пикап трудоустройства (info-икона, сервис-точка, подбор по касанию) + иконка на
// миникарте (51 — грузовик-развозчик, решение владельца). Координаты — замеры.
const Vector3 EMPLOY_PICKUP_POS{2171.0681f, -2252.5254f, 13.3026f};
constexpr int PICKUP_MODEL = 1239;
constexpr PickupType PICKUP_TYPE = 1;
constexpr int JOB_MAP_ICON = 51;

// Грузовик Yankee (модель 456). 4 площадки депо в ровный диагональный ряд, единый
// угол 225.6 (шаг 9.79 м). Тело грузовика лежит вдоль оси места, ⊥ ряду площадок —
// пробы занятости (по SLOT_ANGLE) не кросс-детектят соседей (как у автобуса).
constexpr int TRUCK_MODEL = 456;
constexpr float SLOT_ANGLE = 225.6f;
const Vector3 SLOT_POS[HaulerJobService::SLOT_COUNT] = {
    {2155.2537f, -2289.8650f, 13.6094f},
    {2161.9800f, -2282.7400f, 13.6142f},
    {2168.7000f, -2275.6200f, 13.5707f},
    {2175.4233f, -2268.4973f, 13.6202f},
};

// «Грузовик ещё стоит на своей площадке» (truckOnSpot): XY-дистанция ОРИГИНА до
// точки; занятость точки ЛЮБЫМ ТС для пере-стока — отдельно, многопробно.
constexpr float SPOT_RADIUS = 7.0f;
constexpr float SPOT_OCCUPIED_RADIUS = 3.0f;
constexpr float SPOT_PROBE_OFFSET = 4.0f;

// Точка ВЫГРУЗКИ на базе (куда носить коробки при разгрузке) — замер владельца. Также
// цель стрелки ПОСЛЕДНЕГО чекпоинта плеча «обратно» (доводит к зоне свободной парковки).
const Vector3 BASE_UNLOAD_POS{2174.8723f, -2249.0066f, 13.3039f};

// Центр склад-зоны порта (среднее 6 точек сброса PortJobService) — цель стрелки
// ПОСЛЕДНЕГО чекпоинта плеча «туда»: доводит к зоне погрузки, парковка свободная.
const Vector3 WAREHOUSE_CENTER{2789.6447f, -2456.4956f, 13.6400f};

// Смещение чекпоинта «зад грузовика» назад по оси кузова (Yankee ~7 м). Пересчёт
// от ЖИВОЙ позиции/угла на каждую коробку — грузовик могли переставить.
constexpr float TRUCK_REAR_OFFSET = 4.5f;

// Радиусы чекпоинтов. Все маршрутные точки — race move (RACE_NORMAL со стрелкой), как
// move автобуса; финишных «стоп»-точек нет (парковка свободная); пешие — стандартные.
constexpr float MOVE_CP_RADIUS = 4.0f;
constexpr float BOX_CP_RADIUS = 3.0f;

// Минимальное расстояние «взять -> положить» для ОДНОЙ коробки. Один конец переноски
// задаёт игрок (зад его грузовика), поэтому без этого правила он ставит грузовик
// вплотную к точке выгрузки и стоит сразу в обоих чекпоинтах: переноска вырождается в
// анимации (~1.3 с на коробку), а enter второго чекпоинта вдобавок глохнет о дебаунс
// CheckpointService — игрок стоит в чекпоинте, и «ничего не происходит».
constexpr float MIN_CARRY_DISTANCE = 15.0f;

// Баланс (все именованные, в одном месте).
// Плата за ПРИБЫТИЕ — ВОДИТЕЛЮ, за каждое плечо: доехал в порт и доехал на базу.
// Полный круг даёт ему 2 x PAY_ARRIVAL независимо от того, кто носил коробки.
constexpr std::int64_t PAY_ARRIVAL = 500;
// Плата за коробку — тому, кто её реально нёс (в паре это грузчик, соло — водитель).
// Разгрузка дороже погрузки: она и есть смысл рейса.
constexpr std::int64_t PAY_PER_BOX_LOAD = 30;
constexpr std::int64_t PAY_PER_BOX_UNLOAD = 70;
// Бонус за полный круг — КАЖДОМУ, и ТОЛЬКО в паре: это рычаг, ради которого пару
// собирают (один грузовик — два рабочих места).
constexpr std::int64_t PAIR_FULL_UNLOAD_BONUS = 200;

// Заказы бизнесов. Ставки — те же (прибытие/коробки), сверху идёт ПРЕМИЯ владельца:
// её делят ПОПОЛАМ водитель и грузчик, соло забирает всю. Нечётный остаток достаётся
// водителю: рейс и грузовик на нём.
constexpr int ORDER_PAGE_SIZE = 10; // заказов на странице списка

// Звук прогресса: играет вместе с попапом «что-то засчитано» (коробка, прибытие,
// бонус). На отказы и провалы НЕ вешаем — там свой красный попап.
constexpr std::uint32_t PROGRESS_SOUND = 17803;
constexpr int RESERVE_SECONDS = 30;                 // окно посадки (Reserved)
constexpr int RETURN_SECONDS = 30;                  // окно возврата за руль — ТОЛЬКО фазы езды
// Анти-AFK езды: за рулём без единого зачёта чекпоинта -> увольнение (порог выше
// любого легального плеча: база-порт это ~1.5 км, минуты полторы), с предупреждением
// за 30 с.
constexpr int DRIVE_NO_PROGRESS_SECONDS = 240;
constexpr int DRIVE_NO_PROGRESS_WARN_SECONDS = 210;
// То же для водителя БЕЗ ЦЕЛИ (фаза Idle): грузовик занят, очередь ждёт машину, поэтому
// потолок нужен. Порог щедрее маршрутного: сюда попадает дорога от точки заказа до базы,
// а точки ставит дев — она может оказаться в другом конце города.
constexpr int NO_TARGET_SECONDS = 420;
constexpr int NO_TARGET_WARN_SECONDS = 390;
// Анти-AFK пеших фаз: без сданной коробки -> увольнение (коробка близко, цикл
// «дойти/взять/донести/сдать» ~15-40 с), с предупреждением за 30 с.
constexpr int FOOT_NO_PROGRESS_SECONDS = 120;
constexpr int FOOT_NO_PROGRESS_WARN_SECONDS = 90;
constexpr Milliseconds DEPOT_TICK{1000};

// Анимация коробки (как в порту): взятие/укладка — freeze-поза + таймер удержания,
// несение — SpecialAction_Carry (ходьба с коробкой).
constexpr Milliseconds LIFT_DURATION{1000};
constexpr Milliseconds PUTDOWN_DURATION{300};

// Коробка в руке — те же выверенные /aedit-параметры и модель, что у порта (та же
// правая кисть). Порт держит их приватно; здесь дублируем 3 вектора (см. отчёт).
constexpr int BOX_MODEL = 1220;
const Vector3 BOX_OFFSET{-0.038f, 0.132f, -0.258f};
const Vector3 BOX_ROTATION{-16.0f, 0.0f, 0.0f};
const Vector3 BOX_SCALE{0.6f, 0.513f, 1.232f};

// Экранные попапы — ТОЛЬКО English (как все попапы проекта).
constexpr Milliseconds RESERVE_POPUP_TIME{4000};
constexpr Milliseconds PHASE_POPUP_TIME{3000};
constexpr Milliseconds COUNT_POPUP_TIME{2000}; // счётчик коробок — короткий попап (как зачёт чекпоинта)
constexpr Milliseconds CREDIT_POPUP_TIME{2500};
constexpr Milliseconds BONUS_POPUP_TIME{3500};
constexpr Milliseconds FAIL_POPUP_TIME{3000};
const Colour NEUTRAL_POPUP_COLOUR{0xFF, 0xFF, 0xFF, 0xFF};
const Colour CREDIT_POPUP_COLOUR{0x90, 0xEE, 0x90, 0xFF};
const Colour ERROR_POPUP_COLOUR{0xFF, 0x5A, 0x5A, 0xFF};

// GUI-таймеры обратного отсчёта (ScreenTimerService) — ТОЛЬКО English.
const char *const BOARDING_TIMER_LABEL = "BOARDING";
const char *const RETURN_TIMER_LABEL = "RETURN";

// ЕДИНСТВЕННЫЙ чекпоинт плеча «в порт» — сам порт, у ворот склада (замер владельца).
// Промежуточной разметки нет СОЗНАТЕЛЬНО: дорогу водитель выбирает сам, а раньше её
// диктовали 14 точек — маршрут был рельсами, и любой объезд стоил увольнения за
// «нет прогресса». Зачёт этой точки сразу запускает погрузку; её стрелка ведёт на
// склад-зону, парковка свободная.
const Vector3 PORT_DRIVE_POINT{2745.3328f, -2505.7141f, 13.6646f};

// ЕДИНСТВЕННЫЙ чекпоинт плеча «на базу» — въезд в депо. Зачёт запускает разгрузку, его
// стрелка ведёт на точку выгрузки. Он же цель водителя БЕЗ ЗАДАЧИ (фаза Idle): цель
// рейса выбирают только на базе, и маркер ведёт именно сюда.
const Vector3 BASE_DRIVE_POINT{2218.5100f, -2235.9246f, 13.7202f};

// «Водитель на базе» — радиус вокруг въезда в депо. Покрывает весь двор с запасом:
// дальняя площадка в 83 м, точка выгрузки в 46 м, пикап работы в 50 м. Цель рейса
// выбирается ТОЛЬКО здесь, потому что груз заказа лежит на базе — выбор за полкарты
// означал бы погрузку, до которой ещё ехать. Мера грубая сознательно: гейт про «ты в
// депо», а не про сантиметры парковки.
constexpr float BASE_AREA_RADIUS = 110.0f;

// Куда указывает стрелка чекпоинта плеча — ЦЕЛЬ пешей фазы: точка выгрузки базы
// («обратно») или центр склад-зоны порта («туда»). Доводит к зоне, где грузовик
// паркуется свободно и начинается разгрузка/погрузка.
const Vector3 &phaseTarget(HaulerJobService::Phase phase)
{
    return phase == HaulerJobService::Phase::DriveBack ? BASE_UNLOAD_POS : WAREHOUSE_CENTER;
}

// Горизонтальная (XY) дистанция² — для «грузовик на площадке» (Z игнорируем).
float distanceSq2D(const Vector3 &a, const Vector3 &b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}
} // namespace

HaulerJobSystem::HaulerJobSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_haulerJobService(serviceRegister.getService<HaulerJobService>()),
      m_haulerWalletService(serviceRegister.getService<HaulerWalletService>()),
      m_jobWalletService(serviceRegister.getService<JobWalletService>()),
      m_orderService(serviceRegister.getService<BusinessOrderService>()),
      m_businessService(serviceRegister.getService<BusinessService>()),
      m_inventoryService(serviceRegister.getService<InventoryService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_checkpointService(serviceRegister.getService<CheckpointService>()),
      m_pickupService(serviceRegister.getService<PickupService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_healthService(serviceRegister.getService<PlayerHealthService>()),
      m_timers(serviceRegister.getService<TimerService>()),
      m_screenNoticeService(serviceRegister.getService<ScreenNoticeService>()),
      m_screenTimerService(serviceRegister.getService<ScreenTimerService>()),
      m_mapIconService(serviceRegister.getService<MapIconService>()),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>()),
      m_navLockService(serviceRegister.getService<NavigationLockService>()),
      m_animationService(serviceRegister.getService<PlayerAnimationService>()),
      m_attachmentService(serviceRegister.getService<AttachmentService>()),
      m_audioService(serviceRegister.getService<AudioService>()), m_rng(std::random_device{}())
{
    m_boxSlot.fill(-1); // 0 — валидный слот, дефолт «нет коробки» = -1

    // Загрузка персистентного кошелька развозчика по старту сессии (serial-guard).
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            loadWallet(player, session);
        });

    // Конец сессии (в т.ч. дисконнект) в ЛЮБОЙ фазе: тихий teardown смены (деспавн
    // ведомого грузовика, снятие резерва/очереди, снятие коробки/анимации) + сброс
    // ОЗУ-кэша кошелька (баланс в БД остаётся).
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            onSessionEnd(player);
        });

    // Смерть в смене серверно-авторитетна — немедленное увольнение (депо-правило).
    m_healthService.subscribeDeath(
        [this](IPlayer &player)
        {
            onPlayerDeath(player);
        });

    // Гейт водителя (единственный слой привязки): за руль резервного/ведомого
    // грузовика — только его работник, в pre-stock — никто; за руль СВОЕГО С
    // КОРОБКОЙ — отказ (запрет посадки с грузом). Пассажиров не гейтим. Другие
    // Owner::Work (автобусы/дев-машины) для нас чужие -> возвращаем true.
    m_vehicleService.subscribeDriverGate([this](IPlayer &player, IVehicle &vehicle)
                                         { return onDriverGate(player, vehicle); });

    // /pair — водитель зовёт грузчика в пару. Приглашение подтверждается диалогом у
    // грузчика: связать людей без согласия обоих нельзя.
    serviceRegister.getService<PlayerCommandService>().add(
        "pair", {{PlayerCommandService::Param::Int, "id грузчика"}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            onPairCommand(player, args.getInt(0));
        },
        {}, "позвать грузчика в пару (водитель портового развозчика)",
        PlayerCommandService::HelpCategory::Economy);

    // /target — цель рейса. Команда, а не пикап: водитель выбирает её ЗА РУЛЁМ, стоя на
    // базе, и после каждого сданного рейса возвращается к этому шагу.
    serviceRegister.getService<PlayerCommandService>().add(
        "target", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            onTargetCommand(player);
        },
        {}, "выбрать цель рейса — порт или заказ бизнеса (водитель развозчика)",
        PlayerCommandService::HelpCategory::Economy);

    // /unpair — разойтись. Работу при этом не теряет НИ ОДИН из двоих: водитель
    // возвращается к соло-правилам, грузчик ждёт нового приглашения.
    serviceRegister.getService<PlayerCommandService>().add(
        "unpair", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            onUnpairCommand(player);
        },
        {}, "разойтись с напарником по работе развозчика", PlayerCommandService::HelpCategory::Economy);

    // Универсальный выход с работы (/stopjob): реестр знает, чем игрок занят, а
    // увольняет наш же обработчик пикапа — второй логики увольнения не появляется.
    serviceRegister.getService<JobDismissService>().registerJob(
        "портовый развозчик",
        "Пара распадётся, грузовик (если он за вами) будет снят. Заработок в кошельке развозчика сохранится, "
        "его можно забрать у пикапа работы.",
        [this](int playerId)
        {
            return m_haulerJobService.isWorking(playerId);
        },
        [this](IPlayer &player)
        {
            onFinishWork(player);
        });

    // Кошелёк этой работы — в справочный список (/jobwallet). Только баланс: выдача
    // остаётся на пикапе работы, туда за деньгами и едут.
    serviceRegister.getService<JobWalletService>().registerWallet(
        "портовый развозчик", "пикап работы в депо развозчиков",
        [this](int playerId)
        {
            return m_haulerWalletService.balanceOf(playerId);
        });
}

void HaulerJobSystem::initialize(IComponentList * /*components*/)
{
    m_pickup = m_pickupService.add(
        PICKUP_MODEL, PICKUP_TYPE, EMPLOY_PICKUP_POS,
        [this](IPlayer &player)
        {
            onPickup(player);
        },
        0);

    // Стрим ровно по радару: дальше иконка только прижималась бы к его краю,
    // указывая на то, чего в видимой области ещё нет.
    m_mapIconService.addGlobal(JOB_MAP_ICON, EMPLOY_PICKUP_POS, Colour::White(), MapIconStyle_Global,
                               MapIconService::RADAR_STREAM_DISTANCE);

    // Грузовиков в депо на старте НЕТ: площадки пустуют, машина появляется на
    // площадке только под конкретного работника (см. onReserved).

    // Один общий per-second таймер депо на весь сервер: НИКОГДА не отменяется.
    m_depotTimer = m_timers.setInterval(DEPOT_TICK,
                                        [this]()
                                        {
                                            onDepotTick();
                                        });
}

// ------------------------------------------------------------------ пикап + диалог

void HaulerJobSystem::onPickup(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }

    // Все пункты видны ВСЕГДА (правило проекта): гейт — в обработчике по клику.
    const std::string body = "Начать работу\nЗавершить работу\nЗабрать деньги\nИнформация";
    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Работа — портовый развозчик", body, "Выбрать", "Закрыть"),
        [this, playerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return;
            }
            switch (listItem)
            {
            case 0:
                showRoleChoice(*player);
                break;
            case 1:
                onFinishWork(*player);
                break;
            case 2:
                onWithdrawMoney(*player);
                break;
            case 3:
                showInfo(*player);
                break;
            default:
                break;
            }
        });
}

void HaulerJobSystem::showRoleChoice(IPlayer &player)
{
    const int playerId = player.getID();

    // Пункты видны всегда (правило проекта) — гейт в обработчике по клику.
    const std::string body = "Водитель\tгрузовик выдаётся сразу; цель рейса выбирается за рулём (/target)\n"
                             "Грузчик\tбез грузовика, платят за коробки; работает в паре с водителем";
    m_dialogService.show(
        player, makeDialog(DialogStyle_TABLIST, "Кем устроиться", body, "Выбрать", "Отмена"),
        [this, playerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *chooser = m_core.getPlayers().get(playerId);
            if (!chooser || response != DialogResponse_Left)
            {
                return;
            }
            if (listItem == 0)
            {
                startAsDriver(*chooser);
            }
            else
            {
                startAsLoader(*chooser);
            }
        });
}

void HaulerJobSystem::startAsLoader(IPlayer &player)
{
    const int playerId = player.getID();

    if (!m_sessionService.isActive(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы работать"));
        return;
    }
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из транспорта, чтобы устроиться грузчиком"));
        return;
    }
    if (m_haulerJobService.isWorking(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы уже работаете развозчиком"));
        return;
    }
    if (m_navLockService.isLocked(playerId))
    {
        player.sendClientMessage(
            ERROR_COLOUR, u(fmt::format("Нельзя устроиться грузчиком: {}", m_navLockService.lockReason(playerId))));
        return;
    }
    if (!m_haulerJobService.startLoader(playerId))
    {
        return; // гонка кликов — уже устроен
    }

    // Взнос грузчик не платит: грузовик ему не выдаётся, залогу неоткуда взяться.
    m_navLockService.acquire(playerId, "идёт смена грузчика-развозчика");
    m_animationService.preloadLibrary(player, "CARRY");
    clearCounters(playerId);

    player.sendClientMessage(INFO_COLOUR,
                             u("Вы устроены грузчиком. Ждите приглашения водителя — он позовёт вас командой /pair"));
    m_screenNoticeService.show(player, "hired as loader - wait for a driver", RESERVE_POPUP_TIME,
                               NEUTRAL_POPUP_COLOUR);
}

void HaulerJobSystem::startAsDriver(IPlayer &player)
{
    // Устройство водителем = СРАЗУ грузовик: что везти, водитель решает уже за рулём
    // (/target). Выбор рейса на пикапе оставлял бы человека без машины и с целью.
    if (!driverGatesPass(player))
    {
        return;
    }
    beginDriverShift(player);
}

bool HaulerJobSystem::driverGatesPass(IPlayer &player)
{
    const int playerId = player.getID();

    if (!m_sessionService.isActive(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы работать"));
        return false;
    }
    // Клиенту не доверяем: гейт на СЕРВЕРНОМ стейте.
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из транспорта, чтобы устроиться развозчиком"));
        return false;
    }
    if (m_haulerJobService.isWorking(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы уже работаете развозчиком"));
        return false;
    }
    // Взаимное исключение работ: чекпоинт-слот и лок навигации принадлежат ТЕКУЩЕЙ
    // смене. isWorking(развозчик) выше уже ложно, значит держатель лока — ДРУГАЯ
    // работа.
    if (m_navLockService.isLocked(playerId))
    {
        player.sendClientMessage(
            ERROR_COLOUR, u(fmt::format("Нельзя устроиться развозчиком: {}", m_navLockService.lockReason(playerId))));
        return false;
    }
    return true;
}

bool HaulerJobSystem::beginDriverShift(IPlayer &player)
{
    const int playerId = player.getID();

    const HaulerJobService::StartOutcome outcome = m_haulerJobService.startWork(playerId,
                                                                               [this](int spot)
                                                                               {
                                                                                   return spotClear(spot);
                                                                               });
    if (outcome.result == HaulerJobService::StartResult::AlreadyWorking)
    {
        return false; // гонка кликов: смену успели открыть между гейтом и стартом
    }
    // Лок навигации на ВСЮ смену; освобождается на любом её конце.
    m_navLockService.acquire(playerId, "идёт смена портового развозчика");
    // Предзагрузка либы CARRY заранее (первая укладка/подъём иначе не проиграется).
    m_animationService.preloadLibrary(player, "CARRY");

    if (outcome.result == HaulerJobService::StartResult::Queued)
    {
        player.sendClientMessage(
            INFO_COLOUR,
            u(fmt::format("Свободных грузовиков в депо сейчас нет. Вы в очереди, место {}", outcome.queuePosition)));
        return true;
    }
    // Reserved — свободный стоящий грузовик закреплён за игроком.
    onReserved(player);
    return true;
}

void HaulerJobSystem::onTargetCommand(IPlayer &player)
{
    if (!targetGatesPass(player))
    {
        return;
    }
    showTargetChoice(player);
}

bool HaulerJobSystem::targetGatesPass(IPlayer &player)
{
    const int playerId = player.getID();
    if (m_haulerJobService.roleOf(playerId) != HaulerJobService::Role::Driver ||
        m_haulerJobService.shiftOwnerOf(playerId) != playerId)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u("Цель рейса выбирает водитель развозчика — устройтесь у пикапа депо"));
        return false;
    }
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
    if (phase == HaulerJobService::Phase::Queued || phase == HaulerJobService::Phase::Reserved)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сначала сядьте за руль своего грузовика"));
        return false;
    }
    // Цель меняют ТОЛЬКО без активной задачи: иначе водитель бросал бы начатый рейс на
    // полпути, а взятый заказ висел бы принятым.
    if (phase != HaulerJobService::Phase::Idle)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сначала закончите текущий рейс — цель меняют без задачи"));
        return false;
    }
    // За рулём СВОЕГО грузовика — по серверному getDriver, не по заявлению клиента.
    if (!drivingOwnTruck(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Цель выбирают за рулём своего грузовика"));
        return false;
    }
    // Груз обоих рейсов начинается на базе (заказ лежит здесь, портовый рейс отсюда
    // выезжает) — потому и выбор только здесь.
    if (!atBase(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Цель рейса выбирают на базе — возвращайтесь в депо"));
        return false;
    }
    // Коробку из рук в руки не передают (то же правило, что у /pair): смена меняет
    // плечо, и груз в руках носильщика повис бы.
    if (m_haulerJobService.carryingOf(m_haulerJobService.carrierOf(playerId)))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сначала донесите коробку"));
        return false;
    }
    return true;
}

void HaulerJobSystem::showTargetChoice(IPlayer &player)
{
    const int playerId = player.getID();
    const std::size_t available = availableOrders().size();

    // Пункты видны всегда (правило проекта) — гейт в обработчике по клику.
    const std::string body =
        fmt::format("Рейс в порт\tгруз со склада порта на базу\n"
                    "Заказ бизнеса ({})\tгруз с базы в точку владельца; платят за доставку и премию",
                    available);
    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST, "Цель рейса", body, "Выбрать", "Отмена"),
                         [this, playerId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *chooser = m_core.getPlayers().get(playerId);
                             if (!chooser || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             if (listItem == 0)
                             {
                                 startPortRun(*chooser);
                                 return;
                             }
                             showOrderList(*chooser, 0);
                         });
}

void HaulerJobSystem::startPortRun(IPlayer &player)
{
    // Ре-валидация на клике: пока висел диалог, водитель мог выйти из кабины, уехать с
    // базы или потерять смену.
    if (!targetGatesPass(player))
    {
        return;
    }
    if (!m_haulerJobService.startPortRun(player.getID()))
    {
        return; // гонка кликов: цель уже выбрана
    }
    beginDriveOutPhase(player);
}

std::vector<const BusinessOrderService::Order *> HaulerJobSystem::availableOrders() const
{
    std::vector<const BusinessOrderService::Order *> orders = m_orderService.pool();
    // Точку могли снести вместе с непринятым заказом — везти его некуда, и в списке
    // ему делать нечего (порядок остальных сохраняем: пул уже отсортирован премией).
    orders.erase(std::remove_if(orders.begin(), orders.end(),
                                [this](const BusinessOrderService::Order *order)
                                {
                                    return !m_businessService.getBusiness(order->businessId) ||
                                           m_orderService.boxesLeft(order->id) <= 0;
                                }),
                 orders.end());
    return orders;
}

std::string HaulerJobSystem::orderPointName(const BusinessOrderService::Order &order) const
{
    const BusinessService::Business *business = m_businessService.getBusiness(order.businessId);
    if (!business)
    {
        return fmt::format("точка #{}", order.businessId);
    }
    return fmt::format("{} #{}", m_businessService.typeName(business->type), order.businessId);
}

void HaulerJobSystem::showOrderList(IPlayer &player, int page)
{
    const int playerId = player.getID();
    const std::vector<const BusinessOrderService::Order *> orders = availableOrders();
    if (orders.empty())
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u("Свободных заказов сейчас нет. Возьмите рейс в порт — груз там всегда есть"));
        showTargetChoice(player);
        return;
    }

    const int total = static_cast<int>(orders.size());
    const int pageCount = (total + ORDER_PAGE_SIZE - 1) / ORDER_PAGE_SIZE;
    page = std::clamp(page, 0, pageCount - 1);
    const int first = page * ORDER_PAGE_SIZE;
    const int count = std::min(ORDER_PAGE_SIZE, total - first);

    // Колонки — то, по чему заказ выбирают: куда ехать, далеко ли это от депо и
    // сколько за него доплатят. Остаток коробок показываем у недовезённых заказов:
    // такой заказ вернулся в пул с прогрессом, и вести его быстрее.
    std::string body = "Точка\tОт депо\tПремия\n";
    for (int i = first; i < first + count; ++i)
    {
        const BusinessOrderService::Order &order = *orders[i];
        const BusinessService::Business *business = m_businessService.getBusiness(order.businessId);
        const int metres =
            business ? static_cast<int>(std::sqrt(distanceSq2D(business->entrance, EMPLOY_PICKUP_POS))) : 0;
        const int boxes = m_orderService.boxesLeft(order.id);
        body += fmt::format("{}\t{} м\t{}{}\n", orderPointName(order), metres, Money::text(order.bonus),
                            boxes < BusinessOrderService::BOXES_PER_ORDER
                                ? fmt::format(" ({} кор.)", boxes)
                                : std::string());
    }
    if (pageCount > 1)
    {
        body += fmt::format("» Следующая страница ({}/{})", page + 1, pageCount);
    }
    else
    {
        body.pop_back(); // без строки-пагинации хвостовой перевод строки лишний
    }

    // В обработчик уносим ID, а не указатели: пока висит диалог, заказ могли снять, а
    // пул — перестроить (указатели на элементы удалённого заказа стали бы висячими).
    std::vector<int> ids;
    ids.reserve(static_cast<std::size_t>(count));
    for (int i = first; i < first + count; ++i)
    {
        ids.push_back(orders[static_cast<std::size_t>(i)]->id);
    }

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_TABLIST_HEADERS, fmt::format("Заказы бизнесов {}/{}", page + 1, pageCount), body,
                   "Взять", "Назад"),
        [this, playerId, page, pageCount, count, ids = std::move(ids)](DialogResponse response, int listItem,
                                                                      StringView)
        {
            IPlayer *chooser = m_core.getPlayers().get(playerId);
            if (!chooser)
            {
                return;
            }
            if (response != DialogResponse_Left || listItem < 0 || listItem > count)
            {
                showTargetChoice(*chooser);
                return;
            }
            if (listItem == count) // последняя строка — следующая страница (по кругу)
            {
                showOrderList(*chooser, (page + 1) % pageCount);
                return;
            }
            startAsOrderDriver(*chooser, ids[static_cast<std::size_t>(listItem)]);
        });
}

void HaulerJobSystem::startAsOrderDriver(IPlayer &player, int orderId)
{
    const int playerId = player.getID();
    // Ре-валидация цели на клике: пока висел список, водитель мог выйти из кабины,
    // уехать с базы, начать рейс или потерять смену вовсе.
    if (!targetGatesPass(player))
    {
        return;
    }

    // Ре-валидация заказа: пока висел список, его могли взять, снять или снести вместе
    // с точкой.
    const BusinessOrderService::Order *order = m_orderService.get(orderId);
    if (!order || order->driverId >= 0 || m_orderService.boxesLeft(orderId) <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот заказ уже взяли или он снят"));
        showOrderList(player, 0);
        return;
    }
    if (!m_businessService.getBusiness(order->businessId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Точки этого заказа больше нет"));
        showOrderList(player, 0);
        return;
    }
    // Заказ закрепляем ДО перевода фазы: обратный порядок отдал бы один заказ двоим.
    if (!m_orderService.accept(orderId, playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот заказ только что взял другой водитель"));
        showOrderList(player, 0);
        return;
    }
    // Сообщение о взятом заказе — ДО перевода фазы: дальше говорит уже сама погрузка.
    // Указатель `order` всё ещё живой (accept правит запись на месте, не вставляет).
    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Заказ взят: {}, {} коробок, премия {}", orderPointName(*order),
                                           m_orderService.boxesLeft(orderId), Money::text(order->bonus))));

    if (!m_haulerJobService.startOrderLeg(playerId, orderId))
    {
        m_orderService.release(orderId); // гонка кликов — заказ обратно в пул
        return;
    }
    beginLoadingPhase(player); // сервис уже в Loading; здесь цели, попапы, сообщения
}

void HaulerJobSystem::onFinishWork(IPlayer &player)
{
    const int playerId = player.getID();
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);

    if (phase == HaulerJobService::Phase::NotWorking)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы сейчас не работаете развозчиком"));
        return;
    }
    if (phase == HaulerJobService::Phase::Queued)
    {
        releaseOrderOf(playerId);              // защитно: у ждущего в очереди заказа быть не может
        m_haulerJobService.endShift(playerId); // в очереди грузовика нет — снимать нечего
        clearCounters(playerId);
        m_navLockService.release(playerId);
        player.sendClientMessage(INFO_COLOUR, u("Вы вышли из очереди на грузовик"));
        notifyQueueShift();
        return;
    }
    // Активная смена — штатное увольнение (кошелёк остаётся). Взятый заказ уходит
    // обратно в список с уже засчитанными коробками (teardownShift).
    dismiss(player,
            orderMode(m_haulerJobService.shiftOwnerOf(playerId))
                ? "Смена окончена, заказ возвращён в список. Заработок сохранён в кошельке развозчика"
                : "Смена окончена. Заработок сохранён в кошельке развозчика — заберите через «Забрать деньги»",
            INFO_COLOUR);
}

// ------------------------------------------------------------------ пара

void HaulerJobSystem::onPairCommand(IPlayer &driver, int targetId)
{
    const int driverId = driver.getID();

    if (m_haulerJobService.roleOf(driverId) != HaulerJobService::Role::Driver ||
        m_haulerJobService.shiftOwnerOf(driverId) != driverId)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Звать грузчика может только водитель развозчика в смене"));
        return;
    }
    if (m_haulerJobService.partnerOf(driverId) >= 0)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("У вас уже есть грузчик"));
        return;
    }
    // Коробку из рук в руки не передают: пара меняет носильщика, и груз повис бы.
    if (m_haulerJobService.carryingOf(driverId))
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Сначала донесите коробку, потом зовите грузчика"));
        return;
    }

    IPlayer *target = m_core.getPlayers().get(targetId);
    if (!target || targetId == driverId)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Игрок не найден"));
        return;
    }
    if (m_haulerJobService.roleOf(targetId) != HaulerJobService::Role::Loader)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Этот игрок не устроен грузчиком-развозчиком"));
        return;
    }
    if (m_haulerJobService.partnerOf(targetId) >= 0)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Этот грузчик уже работает в паре"));
        return;
    }

    const PlayerSessionService::Session *driverSession = m_sessionService.get(driverId);
    const PlayerSessionService::Session *loaderSession = m_sessionService.get(targetId);
    if (!driverSession || !loaderSession)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Игрок не авторизован"));
        return;
    }

    // Ники обеих сторон — клиентский текст: цветокоды в сообщение не пускаем.
    const StringView driverName = driver.getName();
    const std::string driverSafe = Encoding::neutralizeColorCodes(std::string_view(driverName.data(), driverName.size()));
    const StringView loaderName = target->getName();
    const std::string loaderSafe = Encoding::neutralizeColorCodes(std::string_view(loaderName.data(), loaderName.size()));

    driver.sendClientMessage(INFO_COLOUR, u(fmt::format("Приглашение отправлено грузчику {}[{}]", loaderSafe, targetId)));

    m_dialogService.show(
        *target,
        makeDialog(DialogStyle_MSGBOX, "Приглашение в пару",
                   fmt::format("Водитель {}[{}] зовёт вас грузчиком в свой рейс.\n\nВы будете носить коробки, он "
                               "— водить. В портовом рейсе вам ${} за погруженную коробку и ${} за разгруженную, "
                               "плюс ${} сверху за полный рейс в паре.\n\nВ рейсе с заказом бизнеса погрузка на "
                               "базе не оплачивается, зато премия владельца делится с вами пополам.",
                               driverSafe, driverId, PAY_PER_BOX_LOAD, PAY_PER_BOX_UNLOAD,
                               PAIR_FULL_UNLOAD_BONUS),
                   "Принять", "Отказаться"),
        [this, driverId, targetId, driverSerial = driverSession->serial,
         loaderSerial = loaderSession->serial](DialogResponse response, int, StringView)
        {
            // Serial-guard на ОБЕ стороны: за время диалога слот мог переиспользоваться.
            const PlayerSessionService::Session *nowDriver = m_sessionService.get(driverId);
            const PlayerSessionService::Session *nowLoader = m_sessionService.get(targetId);
            if (!nowDriver || nowDriver->serial != driverSerial || !nowLoader || nowLoader->serial != loaderSerial)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                if (IPlayer *inviter = m_core.getPlayers().get(driverId))
                {
                    inviter->sendClientMessage(ERROR_COLOUR, u("Грузчик отказался от приглашения"));
                }
                return;
            }
            onPairAccepted(driverId, targetId);
        });
}

void HaulerJobSystem::onPairAccepted(int driverId, int loaderId)
{
    IPlayer *driver = m_core.getPlayers().get(driverId);
    IPlayer *loader = m_core.getPlayers().get(loaderId);
    if (!driver || !loader)
    {
        return;
    }
    // Правила связывания — в сервисе: за время диалога роли/фазы могли измениться.
    if (!m_haulerJobService.makePair(driverId, loaderId))
    {
        loader->sendClientMessage(ERROR_COLOUR, u("Пара не сложилась — водитель уже не в смене или занят"));
        return;
    }

    const StringView driverName = driver->getName();
    const std::string driverSafe = Encoding::neutralizeColorCodes(std::string_view(driverName.data(), driverName.size()));
    const StringView loaderName = loader->getName();
    const std::string loaderSafe = Encoding::neutralizeColorCodes(std::string_view(loaderName.data(), loaderName.size()));

    driver->sendClientMessage(INFO_COLOUR,
                              u(fmt::format("{}[{}] теперь ваш грузчик. Ваше дело — руль и чекпоинты", loaderSafe,
                                            loaderId)));
    loader->sendClientMessage(INFO_COLOUR,
                              u(fmt::format("Вы в паре с водителем {}[{}]. Ваше дело — коробки", driverSafe, driverId)));
    m_screenNoticeService.show(*driver, "loader hired", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
    m_screenNoticeService.show(*loader, "paired with a driver", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);

    // Носильщик сменился: цель коробки уходит от водителя к грузчику. Водитель коробку
    // не нёс (проверено в /pair), поэтому передавать нечего — только цели.
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(driverId);
    if (phase == HaulerJobService::Phase::Loading)
    {
        m_checkpointService.clearForPlayer(*driver);
        showBoxSource(*loader);
    }
    else if (phase == HaulerJobService::Phase::Unloading)
    {
        m_checkpointService.clearForPlayer(*driver);
        refreshUnloadSource(*loader); // грузчик в кабине — цель поставится на выходе
    }
    m_footIdleSeconds[driverId] = 0;
    m_footIdleSeconds[loaderId] = 0;
    m_driveIdleSeconds[driverId] = 0;
}

void HaulerJobSystem::onUnpairCommand(IPlayer &player)
{
    const int playerId = player.getID();
    if (m_haulerJobService.partnerOf(playerId) < 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас нет напарника"));
        return;
    }
    // Носильщик меняется — коробку в руках сначала донести (то же правило, что в /pair).
    if (m_haulerJobService.carryingOf(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сначала донесите коробку, потом расходитесь"));
        return;
    }

    const bool isDriver = m_haulerJobService.roleOf(playerId) == HaulerJobService::Role::Driver;
    splitPair(playerId, isDriver ? "Водитель распустил пару — ждите нового приглашения"
                                 : "Грузчик ушёл из пары — дальше вы работаете один");

    if (isDriver)
    {
        player.sendClientMessage(INFO_COLOUR, u("Вы отпустили грузчика — коробки снова на вас"));
        restoreCarrierTarget(player); // снова носильщик своей смены
    }
    else
    {
        player.sendClientMessage(INFO_COLOUR, u("Вы вышли из пары. Ждите приглашения другого водителя"));
        clearCarryState(player); // цель чужой смены больше не его
    }
    m_footIdleSeconds[playerId] = 0;
    m_driveIdleSeconds[playerId] = 0;
}

void HaulerJobSystem::restoreCarrierTarget(IPlayer &player)
{
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(player.getID());
    if (phase == HaulerJobService::Phase::Loading)
    {
        showBoxSource(player);
    }
    else if (phase == HaulerJobService::Phase::Unloading)
    {
        refreshUnloadSource(player); // в кабине цели нет — поставится на выходе
    }
}

void HaulerJobSystem::splitPair(int playerId, const std::string &noticeForPartner)
{
    const int partner = m_haulerJobService.breakPair(playerId);
    if (partner < 0)
    {
        return;
    }
    IPlayer *other = m_core.getPlayers().get(partner);
    if (!other)
    {
        return; // напарник уже вне игры — его состояние снимет его же teardown
    }

    other->sendClientMessage(ERROR_COLOUR, u(noticeForPartner));

    if (m_haulerJobService.roleOf(partner) == HaulerJobService::Role::Loader)
    {
        // Грузчик остался без смены: незавершённая коробка и цель снимаются.
        clearCarryState(*other);
    }
    else
    {
        restoreCarrierTarget(*other); // водитель снова носит сам
    }
    // Чужое состояние трогаем ТОЛЬКО по делу: водитель может быть в фазе езды или на
    // посадке, и снимать ему чекпоинт/экшен здесь нельзя — это его маркер грузовика.
    m_footIdleSeconds[partner] = 0;
    m_driveIdleSeconds[partner] = 0;
}

void HaulerJobSystem::clearCarryState(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId >= 0 && playerId < MAX_PLAYERS)
    {
        m_timers.cancel(m_pendingTimer[playerId]);
    }
    m_checkpointService.clearForPlayer(player);
    detachBox(player); // no-op, если коробки не было
    if (m_haulerJobService.carryingOf(playerId))
    {
        // Позу и carry снимаем ТОЛЬКО у того, кто реально нёс: setAction на игроке за
        // рулём — лишнее вмешательство в чужое состояние.
        m_animationService.stop(player);
        m_stateService.clearSpecialAction(player);
        m_haulerJobService.clearCarry(playerId);
    }
}

void HaulerJobSystem::onWithdrawMoney(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }

    // Ре-валидация на клике: серверный accountId из сессии, не клиентское предположение.
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы забрать деньги"));
        return;
    }

    if (m_haulerWalletService.balanceOf(playerId) <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Забирать нечего"));
        return;
    }

    const std::int64_t amount = m_haulerWalletService.withdraw(playerId, session->accountId);
    if (amount <= 0)
    {
        return; // гонка кликов — уже забрано
    }

    m_moneyService.giveMoney(player, static_cast<unsigned long long>(amount));
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Вы забрали ${} из кошелька развозчика", amount)));
}

void HaulerJobSystem::showInfo(IPlayer &player)
{
    const int playerId = player.getID();

    std::string body = "Поле\tЗначение\n";
    body += "Суть работы\tГрузовик порт—база: водитель везёт, грузчик носит коробки\n";
    body += "Роли\tводитель (свой грузовик) или грузчик (в паре с водителем)\n";
    body += "Вступительный взнос\tнет\n";
    body += "Устройство\tводителю грузовик выдают сразу; цель рейса выбирают за рулём\n";
    body += fmt::format("Цель рейса\tкоманда /target на базе: порт или заказ бизнеса — сейчас свободно {}\n",
                        availableOrders().size());
    body += "Маршрут\tодин чекпоинт на плечо — сам порт и база. Дорогу выбираете сами\n";
    body += "Заказ бизнеса\tгруз с базы в точку владельца; премия сверху, пополам с грузчиком\n";
    body += "Погрузка заказа\tНЕ оплачивается (груз берут на базе) — платят прибытие, разгрузка и премия\n";
    body += "После рейса\tгрузовик остаётся за вами: вернитесь на базу и выберите цель заново (/target)\n";
    body += fmt::format("Водителю\t${} за прибытие в порт и столько же за прибытие на базу\n", PAY_ARRIVAL);
    body += fmt::format("За коробки\t${} за погруженную и ${} за разгруженную — тому, кто нёс\n",
                        PAY_PER_BOX_LOAD, PAY_PER_BOX_UNLOAD);
    body += fmt::format("Бонус пары\t${} каждому за полный рейс ({}/{}) — только в паре\n", PAIR_FULL_UNLOAD_BONUS,
                        HaulerJobService::BOXES_PER_LEG, HaulerJobService::BOXES_PER_LEG);
    body += "Пара\tводитель зовёт грузчика командой /pair <id>, грузчик подтверждает; разойтись — /unpair\n";
    body += fmt::format("Парк грузовиков\t{}/{} в работе; упёрлись — новые водители ждут в очереди\n",
                        m_haulerJobService.truckCount(), HaulerJobService::MAX_TRUCKS);
    body += fmt::format("Коробки\tпо {} туда и обратно, пешком\n", HaulerJobService::BOXES_PER_LEG);
    body += "Выплата\tкопится в кошельке развозчика; на руки — через «Забрать деньги»\n";
    body += fmt::format("Посадка\t{} секунд сесть за руль отмеченного грузовика\n", RESERVE_SECONDS);
    body += fmt::format("Возврат в смене\t{} секунд вернуться за руль, если вышли по пути\n", RETURN_SECONDS);

    std::string status;
    switch (m_haulerJobService.phaseOf(playerId))
    {
    case HaulerJobService::Phase::NotWorking:
        status = "не работаете";
        break;
    case HaulerJobService::Phase::Queued:
        status = fmt::format("в очереди на грузовик, место {}", m_haulerJobService.queuePositionOf(playerId));
        break;
    case HaulerJobService::Phase::Standby:
    {
        const int partner = m_haulerJobService.partnerOf(playerId);
        if (partner < 0)
        {
            status = "грузчик, ждёте приглашения водителя";
        }
        else
        {
            const HaulerJobService::Phase ownerPhase = m_haulerJobService.phaseOf(partner);
            const bool footPhase = ownerPhase == HaulerJobService::Phase::Loading ||
                                   ownerPhase == HaulerJobService::Phase::Unloading;
            status = fmt::format("грузчик в паре с id {}, {}", partner,
                                 footPhase ? fmt::format("коробок {}/{}", m_haulerJobService.boxCountOf(partner),
                                                         HaulerJobService::BOXES_PER_LEG)
                                           : "водитель в пути");
        }
        break;
    }
    case HaulerJobService::Phase::Reserved:
        status = "грузовик закреплён, идёт посадка";
        break;
    case HaulerJobService::Phase::Idle:
        status = atBase(playerId) ? "за рулём на базе, цель рейса не выбрана (/target)"
                                  : "за рулём без рейса — вернитесь на базу за целью";
        break;
    case HaulerJobService::Phase::DriveOut:
        status = "едет в порт за грузом";
        break;
    case HaulerJobService::Phase::Loading:
        status = fmt::format("грузит {}, коробок {}/{}", orderMode(playerId) ? "заказ на базе" : "на складе порта",
                             m_haulerJobService.boxCountOf(playerId), legBoxes(playerId));
        break;
    case HaulerJobService::Phase::DriveBack:
        status = orderMode(playerId) ? "везёт заказ в точку" : "везёт груз на базу";
        break;
    case HaulerJobService::Phase::Unloading:
        status = fmt::format("разгружает {}, коробок {}/{}", orderMode(playerId) ? "заказ в точке" : "на базе",
                             m_haulerJobService.boxCountOf(playerId), legBoxes(playerId));
        break;
    }
    body += fmt::format("Ваш статус\t{}\n", status);

    // Заказ смены — у водителя свой, у грузчика заказ его напарника.
    if (const BusinessOrderService::Order *order = orderOfShift(m_haulerJobService.shiftOwnerOf(playerId)))
    {
        body += fmt::format("Заказ рейса\t{}, осталось {} коробок, премия {}\n", orderPointName(*order),
                            m_orderService.boxesLeft(order->id), Money::text(order->bonus));
    }

    if (m_haulerJobService.roleOf(playerId) == HaulerJobService::Role::Driver)
    {
        const int partner = m_haulerJobService.partnerOf(playerId);
        body += fmt::format("Ваш грузчик\t{}\n", partner >= 0 ? fmt::format("id {}", partner)
                                                                : std::string("нет — работаете один"));
    }
    body += fmt::format("В кошельке развозчика\t${}", m_haulerWalletService.balanceOf(playerId));

    m_dialogService.show(player,
                         makeDialog(DialogStyle_TABLIST_HEADERS, "Работа портовым развозчиком", body, "Назад", ""),
                         [](DialogResponse, int, StringView)
                         {
                         });
}

// ------------------------------------------------------------------ гейт водителя

bool HaulerJobSystem::onDriverGate(IPlayer &player, IVehicle &vehicle)
{
    const int vid = vehicle.getID();
    // Дешёвый фильтр: наши грузовики — Owner::Work. Прочий транспорт (в т.ч. автобусы
    // и дев-машины — тоже Work) распознаётся ниже по нашему стейту, чужому — true.
    if (m_vehicleService.getOwner(vid) != VehicleService::Owner::Work)
    {
        return true;
    }
    const int playerId = player.getID();

    const int worker = m_haulerJobService.workerOfVehicle(vid);
    if (worker >= 0)
    {
        if (worker == playerId)
        {
            // Свой грузовик. С КОРОБКОЙ в руках за руль не пускаем (запрет посадки с
            // грузом — порт этого не гейтил, здесь закрываем явно).
            if (m_haulerJobService.carryingOf(playerId))
            {
                player.sendClientMessage(ERROR_COLOUR,
                                         u("Сначала донесите коробку — с ней за руль нельзя"));
                return false;
            }
            m_waypointService.clearFor(player); // сел в свой — маркер-указатель больше не нужен
            // Посадка за руль — и есть конец окна посадки: маршрут открывается здесь.
            // Гейт зовётся ПОСЛЕ привязки водителя (getDriver уже наш), поэтому переход
            // опирается на серверный факт. Страховка на случай посадки в обход гейта —
            // в тике (tickWorker, ветка Reserved).
            if (m_haulerJobService.phaseOf(playerId) == HaulerJobService::Phase::Reserved)
            {
                completeBoarding(player);
            }
            return true;
        }
        player.sendClientMessage(ERROR_COLOUR,
                                 u("Это служебный грузовик — за руль пускают только назначенного водителя"));
        return false;
    }
    return true; // не наш грузовик — гейт не наш
}

// ------------------------------------------------------------------ резерв/очередь

void HaulerJobSystem::onReserved(IPlayer &player)
{
    const int playerId = player.getID();
    clearCounters(playerId); // свежее окно посадки

    // Грузовик подаётся ПОД РАБОТНИКА: депо стоит пустым, машина появляется на его
    // площадке только сейчас. Не смогли подать (пул машин полон) — площадку не держим,
    // возвращаем в очередь: занятая площадка без машины заблокировала бы депо.
    if (!spawnForWorker(player))
    {
        m_haulerJobService.requeueTail(playerId);
        player.sendClientMessage(ERROR_COLOUR, u("Грузовик сейчас не подать. Вы возвращены в очередь"));
        return;
    }

    // В фазе посадки цель ровно одна — СВОЙ грузовик: красный чекпоинт-маркер на нём и
    // ничего больше. Маршрут в это время не показываем: до руля он не задача игрока, а
    // две цели сразу только путают, какая из них закрывает окно посадки.
    const int vid = m_haulerJobService.vehicleIdOf(playerId);
    if (IVehicle *truck = m_vehicleService.get(vid))
    {
        m_waypointService.showFor(player, *truck);
    }

    m_screenTimerService.show(player, RESERVE_SECONDS, BOARDING_TIMER_LABEL);
    player.sendClientMessage(
        INFO_COLOUR,
        u(fmt::format("Вам подан грузовик — он отмечен маркером. У вас {} секунд сесть за руль",
                      RESERVE_SECONDS)));
    m_screenNoticeService.show(player, "truck reserved - get in", RESERVE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
}

void HaulerJobSystem::completeBoarding(IPlayer &player)
{
    const int playerId = player.getID();
    m_haulerJobService.completeBoarding(playerId); // Reserved -> Idle (цель ещё не выбрана)
    m_exitSeconds[playerId] = 0;
    m_driveIdleSeconds[playerId] = 0;
    m_waypointService.clearFor(player); // маркер грузовика больше не нужен

    // Площадку отпускаем СРАЗУ, не дожидаясь отъезда: цель рейса водитель выбирает
    // именно здесь, и окно выезда уволило бы его за то, что он читает диалог. Отдать
    // площадку следующему это не мешает — физическую занятость привод считает сам
    // (spotClear), и пока грузовик стоит на ней, в неё никто не заспавнится.
    m_haulerJobService.releaseSpot(playerId);
    m_reserveSeconds[playerId] = 0;
    m_screenTimerService.hide(player);

    player.sendClientMessage(INFO_COLOUR,
                             u("Вы за рулём. Выберите цель рейса командой /target — порт или заказ бизнеса"));
    m_screenNoticeService.show(player, "choose your run: /target", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
}

void HaulerJobSystem::pumpQueue()
{
    bool promoted = false;
    for (;;)
    {
        const HaulerJobService::Promotion promotion = m_haulerJobService.promoteQueue(
            [this](int spot)
            {
                return spotClear(spot);
            });
        if (promotion.playerId < 0)
        {
            break; // очередь пуста / свободных стоящих грузовиков нет
        }
        IPlayer *next = m_core.getPlayers().get(promotion.playerId);
        if (!next)
        {
            // Продвинутый работник пропал: снять резерв. Заказ отпускаем защитно — цель
            // рейса берут за рулём, поэтому у ждущего в очереди её ещё нет.
            releaseOrderOf(promotion.playerId);
            m_haulerJobService.endShift(promotion.playerId);
            clearCounters(promotion.playerId);
            m_navLockService.release(promotion.playerId);
            continue;
        }
        onReserved(*next);
        promoted = true;
    }
    if (promoted)
    {
        notifyQueueShift();
    }
}

void HaulerJobSystem::notifyQueueShift()
{
    const std::vector<int> queued = m_haulerJobService.queuedPlayers();
    for (int i = 0; i < static_cast<int>(queued.size()); ++i)
    {
        IPlayer *waiting = m_core.getPlayers().get(queued[i]);
        if (!waiting)
        {
            continue;
        }
        if (i == 0)
        {
            waiting->sendClientMessage(INFO_COLOUR, u("Очередь на грузовик продвинулась — вы следующий"));
        }
        else
        {
            waiting->sendClientMessage(INFO_COLOUR,
                                       u(fmt::format("Очередь на грузовик продвинулась, ваше место {}", i + 1)));
        }
    }
}

// ------------------------------------------------------------------ маршрут (езда)

void HaulerJobSystem::showDriveCheckpoint(IPlayer &player)
{
    const int playerId = player.getID();
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);

    // Водитель БЕЗ ЗАДАЧИ: единственная цель — база, там выбирают рейс (/target). Уже на
    // базе — цели нет вовсе, чекпоинт в двух шагах только мешал бы.
    if (phase == HaulerJobService::Phase::Idle)
    {
        if (atBase(playerId))
        {
            m_checkpointService.clearRaceForPlayer(player);
            return;
        }
        m_checkpointService.setRaceForPlayer(player, RaceCheckpointType::RACE_FINISH, BASE_DRIVE_POINT,
                                             Vector3{0.0f, 0.0f, 0.0f}, MOVE_CP_RADIUS,
                                             [this](IPlayer &p)
                                             {
                                                 onDriveCheckpointEnter(p);
                                             });
        return;
    }

    if (phase == HaulerJobService::Phase::DriveOut)
    {
        // В порт — одна точка. RACE_NORMAL: стрелка ведёт на склад-зону, где начнётся
        // погрузка (парковку игрок выбирает сам).
        m_checkpointService.setRaceForPlayer(player, RaceCheckpointType::RACE_NORMAL, PORT_DRIVE_POINT,
                                             phaseTarget(phase), MOVE_CP_RADIUS,
                                             [this](IPlayer &p)
                                             {
                                                 onDriveCheckpointEnter(p);
                                             });
        return;
    }
    if (phase != HaulerJobService::Phase::DriveBack)
    {
        return; // защитно (не фаза езды)
    }

    if (orderMode(playerId))
    {
        // Заказ везут в точку бизнеса. RACE_FINISH: стрелке некуда указывать, дальше
        // рейса нет (nextPosition обязан быть пустым, иначе клиент покажет стрелку).
        Vector3 target;
        if (!orderDriveTarget(playerId, target))
        {
            dismiss(player, "Заказ снят — рейс отменён, заработок сохранён", ERROR_COLOUR);
            return;
        }
        m_checkpointService.setRaceForPlayer(player, RaceCheckpointType::RACE_FINISH, target,
                                             Vector3{0.0f, 0.0f, 0.0f}, MOVE_CP_RADIUS,
                                             [this](IPlayer &p)
                                             {
                                                 onDriveCheckpointEnter(p);
                                             });
        return;
    }
    // На базу — одна точка, стрелка на точку выгрузки.
    m_checkpointService.setRaceForPlayer(player, RaceCheckpointType::RACE_NORMAL, BASE_DRIVE_POINT,
                                         phaseTarget(phase), MOVE_CP_RADIUS,
                                         [this](IPlayer &p)
                                         {
                                             onDriveCheckpointEnter(p);
                                         });
}

void HaulerJobSystem::onDriveCheckpointEnter(IPlayer &player)
{
    const int playerId = player.getID();
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
    // Зачёт ТОЛЬКО за рулём СВОЕГО грузовика (серверный getDriver, не заявление клиента).
    if (!drivingOwnTruck(playerId))
    {
        return;
    }

    // Приехал на базу без задачи: чекпоинт снимаем и напоминаем про выбор цели. Простой
    // считается с нуля — дорога сюда и была прогрессом.
    if (phase == HaulerJobService::Phase::Idle)
    {
        m_checkpointService.clearRaceForPlayer(player);
        m_driveIdleSeconds[playerId] = 0;
        player.sendClientMessage(INFO_COLOUR, u("Вы на базе. Выберите цель рейса командой /target"));
        m_screenNoticeService.show(player, "at base - /target", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
        return;
    }
    // Плечо езды одноточечное: доехал — сразу пешая фаза.
    if (phase == HaulerJobService::Phase::DriveOut)
    {
        beginLoadingPhase(player);
        return;
    }
    if (phase == HaulerJobService::Phase::DriveBack)
    {
        beginUnloadingPhase(player);
    }
}

void HaulerJobSystem::beginLoadingPhase(IPlayer &player)
{
    const int playerId = player.getID();
    m_haulerJobService.beginLoading(playerId); // DriveOut -> Loading, boxCount=0
    m_checkpointService.clearRaceForPlayer(player);
    m_screenTimerService.hide(player); // в пешей фазе окна езды нет
    m_footIdleSeconds[playerId] = 0;

    const int boxes = legBoxes(playerId);
    const bool orders = orderMode(playerId);

    if (orders)
    {
        const BusinessOrderService::Order *order = orderOfShift(playerId);
        if (!order)
        {
            dismiss(player, "Заказ снят — рейс отменён, заработок сохранён", ERROR_COLOUR);
            return;
        }
        // Что именно везём — сразу и одним сообщением: состав заказа игрок больше нигде
        // не увидит, а от него зависит, сколько ходок делать.
        std::string composition;
        for (const BusinessOrderService::Item &item : order->items)
        {
            composition += fmt::format("{}{} — {} шт.", composition.empty() ? "" : ", ",
                                       m_inventoryService.itemName(item.itemType), item.quantity);
        }
        player.sendClientMessage(INFO_COLOUR,
                                 u(fmt::format("Груз для {} лежит на базе: загрузите {} коробок с точки погрузки",
                                               orderPointName(*order), boxes)));
        player.sendClientMessage(INFO_COLOUR, u(fmt::format("В заказе: {}", composition)));
        player.sendClientMessage(INFO_COLOUR, u("Погрузка на базе не оплачивается — платят за доставку в точку"));
        m_screenNoticeService.show(player, fmt::format("base: load {} boxes for the order", boxes), PHASE_POPUP_TIME,
                                   NEUTRAL_POPUP_COLOUR);
        // Плата за прибытие в заказе одна — за приезд В ТОЧКУ (см. beginUnloadingPhase):
        // плеча в порт нет, и платить за посадку в депо не за что.
    }
    else
    {
        player.sendClientMessage(
            INFO_COLOUR, u(fmt::format("Вы прибыли в порт. Загрузите {} коробок со склада в грузовик", boxes)));
        m_screenNoticeService.show(player, fmt::format("port: load {} boxes into truck", boxes), PHASE_POPUP_TIME,
                                   NEUTRAL_POPUP_COLOUR);
        // Доехал до порта — плата за плечо. Она принадлежит ВОДИТЕЛЮ: это его работа,
        // коробки оплачиваются отдельно и носильщику.
        creditParticipant(playerId, PAY_ARRIVAL, fmt::format("port reached +${}", PAY_ARRIVAL), CREDIT_POPUP_TIME);
    }

    // Коробки грузит НОСИЛЬЩИК смены: соло-водитель сам, в паре — грузчик.
    const int carrierId = m_haulerJobService.carrierOf(playerId);
    if (IPlayer *carrier = m_core.getPlayers().get(carrierId))
    {
        if (carrierId != playerId)
        {
            carrier->sendClientMessage(
                INFO_COLOUR, u(orders ? fmt::format("Грузовик под заказ на базе. Загрузите {} коробок", boxes)
                                      : fmt::format("Грузовик в порту. Загрузите {} коробок со склада", boxes)));
            m_screenNoticeService.show(*carrier, orders ? "base: load the truck" : "port: load the truck",
                                       PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
            m_footIdleSeconds[carrierId] = 0;
        }
        showBoxSource(*carrier);
    }
}

void HaulerJobSystem::beginDriveBackPhase(IPlayer &player)
{
    const int playerId = player.getID();
    m_haulerJobService.beginDriveBack(playerId); // Loading -> DriveBack, driveIndex=0
    m_checkpointService.clearForPlayer(player);
    m_driveIdleSeconds[playerId] = 0;
    m_exitSeconds[playerId] = 0;
    if (orderMode(playerId))
    {
        const BusinessOrderService::Order *order = orderOfShift(playerId);
        if (!order)
        {
            dismiss(player, "Заказ снят — рейс отменён, заработок сохранён", ERROR_COLOUR);
            return;
        }
        player.sendClientMessage(INFO_COLOUR,
                                 u(fmt::format("Груз в кузове. Везите его в {}", orderPointName(*order))));
        m_screenNoticeService.show(player, "deliver the order", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
    }
    else
    {
        m_screenNoticeService.show(player, "drive back to base", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
    }
    showDriveCheckpoint(player); // плечо BACK, индекс 0 (выезд из зоны склада)
}

void HaulerJobSystem::beginUnloadingPhase(IPlayer &player)
{
    const int playerId = player.getID();
    // Заказ мог быть снят, пока водитель ехал (владелец отказался от точки, дев её
    // снёс): выгружать некуда, и узнать об этом лучше на подъезде, а не на первой
    // коробке. Деньги за путь уже в кошельке, они остаются.
    if (orderMode(playerId) && !orderOfShift(playerId))
    {
        dismiss(player, "Заказ снят, пока вы ехали — рейс отменён, заработок сохранён", ERROR_COLOUR);
        return;
    }
    m_haulerJobService.beginUnloading(playerId); // DriveBack -> Unloading, boxCount=0
    m_checkpointService.clearRaceForPlayer(player);
    m_screenTimerService.hide(player);
    m_footIdleSeconds[playerId] = 0;

    const int boxes = legBoxes(playerId);
    const bool orders = orderMode(playerId);
    if (orders)
    {
        player.sendClientMessage(
            INFO_COLOUR, u(fmt::format("Вы у точки. Занесите {} коробок ВНУТРЬ, к прилавку — вход отмечен пикапом",
                                       boxes)));
        m_screenNoticeService.show(player, fmt::format("deliver {} boxes to the counter", boxes), PHASE_POPUP_TIME,
                                   NEUTRAL_POPUP_COLOUR);
    }
    else
    {
        player.sendClientMessage(INFO_COLOUR,
                                 u(fmt::format("Вы на базе. Разгрузите {} коробок на точку выгрузки", boxes)));
        m_screenNoticeService.show(player, fmt::format("base: unload {} boxes", boxes), PHASE_POPUP_TIME,
                                   NEUTRAL_POPUP_COLOUR);
    }
    // Доехал до места разгрузки — плата за плечо (см. beginLoadingPhase). В заказе это
    // ЕДИНСТВЕННОЕ прибытие: плеча в порт там нет.
    creditParticipant(playerId, PAY_ARRIVAL,
                      fmt::format("{} +${}", orders ? "point reached" : "base reached", PAY_ARRIVAL),
                      CREDIT_POPUP_TIME);
    // Источник коробки — зад грузовика. Ставить его СЕЙЧАС нельзя: зачёт последнего
    // чекпоинта происходит за рулём, и точка примёрзла бы к месту зачёта, а не к месту
    // парковки. Пока носильщик в кабине — цели нет (коробку берут только пешком);
    // поставит тик, как только он выйдет (refreshUnloadSource).
    const int carrierId = m_haulerJobService.carrierOf(playerId);
    if (IPlayer *carrier = m_core.getPlayers().get(carrierId))
    {
        if (carrierId != playerId)
        {
            carrier->sendClientMessage(INFO_COLOUR,
                                       u(fmt::format("Грузовик на базе. Разгрузите {} коробок на точку выгрузки",
                                                     HaulerJobService::BOXES_PER_LEG)));
            m_screenNoticeService.show(*carrier, "base: unload the truck", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
            m_footIdleSeconds[carrierId] = 0;
        }
        refreshUnloadSource(*carrier);
    }
}

void HaulerJobSystem::beginDriveOutPhase(IPlayer &player)
{
    const int playerId = player.getID();
    m_checkpointService.clearForPlayer(player);
    m_driveIdleSeconds[playerId] = 0;
    m_exitSeconds[playerId] = 0;
    player.sendClientMessage(INFO_COLOUR, u("Цель — порт. Езжайте на склад за грузом"));
    m_screenNoticeService.show(player, "drive to the port", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
    if (const int partner = m_haulerJobService.partnerOf(playerId); partner >= 0)
    {
        if (IPlayer *loader = m_core.getPlayers().get(partner))
        {
            loader->sendClientMessage(INFO_COLOUR, u("Водитель взял рейс в порт — вас ждёт погрузка на складе"));
        }
    }
    showDriveCheckpoint(player);
}

void HaulerJobSystem::finishRunPhase(IPlayer &player)
{
    const int playerId = player.getID();
    m_haulerJobService.finishRun(playerId); // Unloading -> Idle
    m_checkpointService.clearForPlayer(player);
    m_driveIdleSeconds[playerId] = 0;
    m_exitSeconds[playerId] = 0;
    // Грузовик остаётся за водителем: кончился РЕЙС, а не смена. Следующую цель он
    // выбирает заново — так один и тот же грузовик ходит и в порт, и по заказам.
    player.sendClientMessage(INFO_COLOUR, u("Рейс сдан. Грузовик остаётся за вами — выберите цель командой /target"));
    showIdleTarget(player);
}

void HaulerJobSystem::showIdleTarget(IPlayer &player)
{
    const int playerId = player.getID();
    showDriveCheckpoint(player); // в Idle это либо чекпоинт базы, либо снятая цель
    if (atBase(playerId))
    {
        m_screenNoticeService.show(player, "choose your run: /target", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
        return;
    }
    player.sendClientMessage(INFO_COLOUR, u("Цель рейса выбирают на базе — вернитесь в депо"));
    m_screenNoticeService.show(player, "return to base", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
}

bool HaulerJobSystem::atBase(int playerId) const
{
    // По ПРИНЯТОЙ сервером позиции (PlayerLocationService ловит и телепорт-рывки):
    // «я на базе» подделать нельзя.
    return distanceSq2D(m_locationService.getPosition(playerId), BASE_DRIVE_POINT) <
           BASE_AREA_RADIUS * BASE_AREA_RADIUS;
}

// ------------------------------------------------- заказ бизнеса в смене (Orders)

bool HaulerJobSystem::orderMode(int shiftOwnerId) const
{
    return shiftOwnerId >= 0 && m_haulerJobService.modeOf(shiftOwnerId) == HaulerJobService::Mode::Orders;
}

const BusinessOrderService::Order *HaulerJobSystem::orderOfShift(int shiftOwnerId) const
{
    if (!orderMode(shiftOwnerId))
    {
        return nullptr;
    }
    const int orderId = m_haulerJobService.orderIdOf(shiftOwnerId);
    if (orderId <= 0)
    {
        return nullptr;
    }
    const BusinessOrderService::Order *order = m_orderService.get(orderId);
    // Заказ мог пропасть между двумя коробками (снос точки девом), и точка вместе с
    // ним: ни ехать, ни выгружать тогда некуда — вызывающий закрывает рейс.
    if (!order || !m_businessService.getBusiness(order->businessId))
    {
        return nullptr;
    }
    return order;
}

bool HaulerJobSystem::orderDriveTarget(int shiftOwnerId, Vector3 &out) const
{
    const BusinessOrderService::Order *order = orderOfShift(shiftOwnerId);
    if (!order)
    {
        return false;
    }
    const BusinessService::Business *business = m_businessService.getBusiness(order->businessId);
    if (!business)
    {
        return false;
    }
    out = business->entrance; // ехать — ко ВХОДУ точки, коробки носят уже внутрь
    return true;
}

bool HaulerJobSystem::orderDropPoint(int shiftOwnerId, Vector3 &out) const
{
    const BusinessOrderService::Order *order = orderOfShift(shiftOwnerId);
    if (!order)
    {
        return false;
    }
    const BusinessService::Business *business = m_businessService.getBusiness(order->businessId);
    if (!business || !m_businessService.catalogValid(business->type, business->interiorIndex))
    {
        return false;
    }
    const BusinessService::CatalogEntry &entry =
        m_businessService.catalog(business->type)[static_cast<std::size_t>(business->interiorIndex)];
    // Товар кладут к ПРИЛАВКУ. Прилавок замерен не у каждого интерьера (нулевой вектор
    // — интерьерных координат (0,0,0) не бывает); там кладём к точке появления внутри:
    // снабжать нужно ЛЮБУЮ точку, иначе владельцы части интерьеров остаются без
    // доставки вовсе.
    const bool measured = entry.counter.x != 0.0f || entry.counter.y != 0.0f || entry.counter.z != 0.0f;
    out = measured ? entry.counter : entry.insideSpawn;
    return true;
}

int HaulerJobSystem::legBoxes(int shiftOwnerId) const
{
    const BusinessOrderService::Order *order = orderOfShift(shiftOwnerId);
    if (!order)
    {
        return HaulerJobService::BOXES_PER_LEG;
    }
    // ОСТАТОК заказа: сорванный заказ возвращается в пул с прогрессом, и следующий
    // водитель грузит и везёт только недовезённое.
    return std::clamp(m_orderService.boxesLeft(order->id), 1, BusinessOrderService::BOXES_PER_ORDER);
}

bool HaulerJobSystem::deliverOrderBox(int shiftOwnerId)
{
    const BusinessOrderService::Order *order = orderOfShift(shiftOwnerId);
    if (!order)
    {
        return false;
    }
    const int orderId = order->id;
    const int businessId = order->businessId;
    // Содержимое читаем ДО зачёта: deliverBox сдвигает прогресс, и «следующая коробка»
    // станет уже другой.
    const std::vector<BusinessOrderService::Item> contents = m_orderService.nextBoxContents(orderId);
    const bool finished = m_orderService.deliverBox(orderId);
    for (const BusinessOrderService::Item &item : contents)
    {
        // Склад точки клампится потолком товара сам; переполнения не бывает — состав
        // заказа посчитан по свободному месту, а продажи место только освобождают.
        m_businessService.addStock(businessId, item.itemType, item.quantity);
    }
    return finished;
}

void HaulerJobSystem::completeOrder(IPlayer &driver, IPlayer &carrier)
{
    const int driverId = driver.getID();
    const int carrierId = carrier.getID();
    const int orderId = m_haulerJobService.orderIdOf(driverId);
    const BusinessOrderService::Order *order = m_orderService.get(orderId);
    const std::int64_t bonus = order ? order->bonus : 0;
    const std::string point = order ? orderPointName(*order) : std::string("точка");
    const int partner = m_haulerJobService.partnerOf(driverId);

    // Заказ снимаем ДО выплат и до конца смены: teardown иначе вернул бы уже довезённый
    // заказ в пул, и его повезли бы второй раз.
    m_haulerJobService.clearOrder(driverId);
    m_orderService.remove(orderId);

    // Премия делится ПОПОЛАМ между водителем и грузчиком; соло забирает всю. Нечётный
    // остаток — водителю: рейс и грузовик на нём.
    if (bonus > 0)
    {
        const std::int64_t loaderShare = partner >= 0 ? bonus / 2 : 0;
        const std::int64_t driverShare = bonus - loaderShare;
        creditParticipant(driverId, driverShare, fmt::format("order bonus +${}", driverShare), BONUS_POPUP_TIME);
        if (partner >= 0)
        {
            creditParticipant(partner, loaderShare, fmt::format("order bonus +${}", loaderShare), BONUS_POPUP_TIME);
        }
    }

    const std::string done =
        fmt::format("Заказ доставлен: {}. Премия {} — {}", point, Money::text(bonus),
                    partner >= 0 ? "пополам с напарником" : "целиком вам");
    driver.sendClientMessage(INFO_COLOUR, u(done));
    if (carrierId != driverId)
    {
        carrier.sendClientMessage(INFO_COLOUR, u(done));
    }

    // СМЕНА НЕ КОНЧАЕТСЯ: грузовик остаётся за водителем, пара — в силе. Кончился РЕЙС,
    // и дальше водитель выбирает цель заново — тем же переходом, что портовый рейс
    // (finishRunPhase). Он стоит у точки бизнеса, поэтому Idle приведёт его на базу.
    finishRunPhase(driver);
}

void HaulerJobSystem::releaseOrderOf(int shiftOwnerId)
{
    const int orderId = m_haulerJobService.orderIdOf(shiftOwnerId);
    if (orderId <= 0)
    {
        return;
    }
    m_haulerJobService.clearOrder(shiftOwnerId);
    // Заказ возвращается в пул КАК ЕСТЬ — с премией и уже засчитанными коробками:
    // довезённое лежит у точки, повторно его не привезут.
    m_orderService.release(orderId);
}

// ---------------------------------------------------- пешая переноска коробки

void HaulerJobSystem::showBoxSource(IPlayer &player)
{
    const int playerId = player.getID();
    const int owner = m_haulerJobService.shiftOwnerOf(playerId);
    if (owner < 0 || m_haulerJobService.carrierOf(owner) != playerId)
    {
        return; // не носильщик этой смены (водитель в паре коробки не трогает)
    }
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(owner);
    if (phase == HaulerJobService::Phase::Loading)
    {
        if (orderMode(owner))
        {
            // Заказ грузят НА БАЗЕ, с той же точки, куда портовый рейс сдаёт груз: это
            // и есть погрузочная площадка депо. Обратный путь (точка -> кузов) держит
            // MIN_CARRY_DISTANCE, см. onBoxPickup.
            m_checkpointService.setForPlayer(player, BASE_UNLOAD_POS, BOX_CP_RADIUS,
                                             [this](IPlayer &p)
                                             {
                                                 onBoxCheckpointEnter(p);
                                             });
            return;
        }
        // Погрузка: берут со СЛУЧАЙНОЙ точки склада порта (общий источник координат),
        // но не ближе MIN_CARRY_DISTANCE к кузову — иначе грузовик, подогнанный вплотную
        // к точке склада, убирает переноску целиком.
        Vector3 rear;
        if (!backOfTruck(owner, rear))
        {
            dismissShiftOwner(owner, "Грузовик пропал — смена окончена");
            return;
        }
        m_checkpointService.setForPlayer(player, randomWarehousePoint(rear), BOX_CP_RADIUS,
                                         [this](IPlayer &p)
                                         {
                                             onBoxCheckpointEnter(p);
                                         });
    }
    else if (phase == HaulerJobService::Phase::Unloading)
    {
        // Разгрузка: берут у ЗАДА грузовика (пересчёт от живой позиции/угла).
        Vector3 rear;
        if (!backOfTruck(owner, rear))
        {
            dismissShiftOwner(owner, "Грузовик пропал — смена окончена");
            return;
        }
        m_checkpointService.setForPlayer(player, rear, BOX_CP_RADIUS,
                                         [this](IPlayer &p)
                                         {
                                             onBoxCheckpointEnter(p);
                                         });
    }
}

void HaulerJobSystem::showBoxDest(IPlayer &player)
{
    const int playerId = player.getID();
    const int owner = m_haulerJobService.shiftOwnerOf(playerId);
    if (owner < 0 || m_haulerJobService.carrierOf(owner) != playerId)
    {
        return;
    }
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(owner);
    if (phase == HaulerJobService::Phase::Loading)
    {
        // Погрузка: несут к ЗАДУ грузовика (пересчёт на каждую коробку).
        Vector3 rear;
        if (!backOfTruck(owner, rear))
        {
            dismissShiftOwner(owner, "Грузовик пропал — смена окончена");
            return;
        }
        m_checkpointService.setForPlayer(player, rear, BOX_CP_RADIUS,
                                         [this](IPlayer &p)
                                         {
                                             onBoxCheckpointEnter(p);
                                         });
    }
    else if (phase == HaulerJobService::Phase::Unloading)
    {
        if (orderMode(owner))
        {
            // Заказ несут ВНУТРЬ точки, к прилавку. Чекпоинт стоит в координатах
            // интерьера: попасть в них можно только войдя в бизнес через его пикап, а
            // «тот ли это бизнес» проверяется по виртуальному миру при входе в чекпоинт
            // (onBoxCheckpointEnter) — интерьер у всех точек одного типа общий.
            Vector3 drop;
            if (!orderDropPoint(owner, drop))
            {
                dismissShiftOwner(owner, "Заказ снят — рейс отменён, заработок сохранён");
                return;
            }
            m_checkpointService.setForPlayer(player, drop, BOX_CP_RADIUS,
                                             [this](IPlayer &p)
                                             {
                                                 onBoxCheckpointEnter(p);
                                             });
            return;
        }
        // Разгрузка: несут на точку выгрузки базы.
        m_checkpointService.setForPlayer(player, BASE_UNLOAD_POS, BOX_CP_RADIUS,
                                         [this](IPlayer &p)
                                         {
                                             onBoxCheckpointEnter(p);
                                         });
    }
}

void HaulerJobSystem::onBoxCheckpointEnter(IPlayer &player)
{
    const int playerId = player.getID();
    // Коробки — дело НОСИЛЬЩИКА смены: соло-водителя либо грузчика в паре. Водитель в
    // паре сюда не проходит, даже если встанет в чекпоинт.
    const int owner = m_haulerJobService.shiftOwnerOf(playerId);
    if (owner < 0 || m_haulerJobService.carrierOf(owner) != playerId)
    {
        return;
    }
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(owner);
    if (phase != HaulerJobService::Phase::Loading && phase != HaulerJobService::Phase::Unloading)
    {
        return; // смена не в пешей фазе — событие не наше
    }
    // Коробку берут/кладут ТОЛЬКО пешком (серверный стейт) — не сидя в грузовике.
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        return;
    }
    if (m_haulerJobService.carryingOf(playerId))
    {
        // Приёмник заказа стоит в координатах ИНТЕРЬЕРА, а интерьер у всех точек одного
        // типа общий — различаются они только виртуальным миром. Без этой проверки
        // коробку можно было бы сдать в ЛЮБОМ похожем магазине, не доехав до заказчика.
        // Мир игрока — серверный факт (PlayerLocationService), а не заявление клиента.
        if (orderMode(owner) && phase == HaulerJobService::Phase::Unloading)
        {
            const BusinessOrderService::Order *order = orderOfShift(owner);
            if (!order || m_locationService.getVirtualWorld(playerId) !=
                              BusinessService::VW_BASE + order->businessId)
            {
                return; // это не интерьер точки заказа — сдавать некуда
            }
        }
        onBoxDrop(player); // несёт коробку -> это чекпоинт-приёмник
    }
    else
    {
        onBoxPickup(player); // без коробки -> это чекпоинт-источник
    }
}

void HaulerJobSystem::onBoxPickup(IPlayer &player)
{
    const int playerId = player.getID();
    const int owner = m_haulerJobService.shiftOwnerOf(playerId);
    if (owner < 0)
    {
        return;
    }

    // Переноска обязана быть переноской. Один её конец задаёт игрок (зад грузовика),
    // и подогнав грузовик вплотную к точке базы он стоял бы сразу в обоих чекпоинтах.
    // Коробку не выдаём: руки свободны, отогнать грузовик можно сразу (на погрузке в
    // порту то же правило решается выбором дальней точки склада — randomWarehousePoint).
    //
    // Пара точек одна и та же в двух случаях: разгрузка портового рейса (кузов -> точка
    // базы) и погрузка заказа (точка базы -> кузов).
    const HaulerJobService::Phase ownerPhase = m_haulerJobService.phaseOf(owner);
    const bool baseDockLeg = ownerPhase == HaulerJobService::Phase::Unloading
                                 ? !orderMode(owner)
                                 : ownerPhase == HaulerJobService::Phase::Loading && orderMode(owner);
    Vector3 rear;
    if (baseDockLeg && backOfTruck(owner, rear) &&
        distanceSq2D(rear, BASE_UNLOAD_POS) < MIN_CARRY_DISTANCE * MIN_CARRY_DISTANCE)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Грузовик стоит вплотную к точке базы — отгоните его на {} м",
                                               static_cast<int>(MIN_CARRY_DISTANCE))));
        return;
    }

    m_checkpointService.clearForPlayer(player); // ровно одна активная цель
    m_haulerJobService.beginCarry(playerId);    // carrying=true (авторитетно, даже если attach=-1)
    attachBox(player);                          // ставит m_boxSlot (может быть -1: все слоты заняты)

    // Анимация подъёма (freeze — сыграть один раз и застыть); несение включится в
    // onLiftFinished (по таймеру).
    const AnimationData liftAnim(4.1f, false, true, true, true, 0, "CARRY", "LIFTUP05");
    m_animationService.play(player, liftAnim, true);

    m_pendingTimer[playerId] = m_timers.setPlayerTimeout(player, LIFT_DURATION,
                                                         [this](IPlayer &p)
                                                         {
                                                             onLiftFinished(p);
                                                         });
}

void HaulerJobSystem::onLiftFinished(IPlayer &player)
{
    const int playerId = player.getID();
    // Одноразовый таймер уже сработал: хэндл гасим СРАЗУ, чтобы teardown, вызванный
    // из этого же колбэка, не пытался отменить таймер, внутри которого мы находимся.
    if (playerId >= 0 && playerId < MAX_PLAYERS)
    {
        m_pendingTimer[playerId] = TimerService::Handle{};
    }
    const int owner = m_haulerJobService.shiftOwnerOf(playerId);
    if (owner < 0)
    {
        return; // уволился/распалась пара во время подъёма
    }
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(owner);
    if (phase != HaulerJobService::Phase::Loading && phase != HaulerJobService::Phase::Unloading)
    {
        return;
    }
    if (!m_haulerJobService.carryingOf(playerId))
    {
        return; // защитно
    }
    m_animationService.stop(player);                              // снять застывшую позу подъёма
    m_stateService.setSpecialAction(player, SpecialAction_Carry); // ходьба с коробкой
    showBoxDest(player);
}

void HaulerJobSystem::onBoxDrop(IPlayer &player)
{
    const int playerId = player.getID();
    m_checkpointService.clearForPlayer(player);
    m_stateService.clearSpecialAction(player); // выйти из carry перед укладкой

    const AnimationData putdownAnim(4.1f, false, true, true, true, 0, "CARRY", "PUTDWN05");
    m_animationService.play(player, putdownAnim, true);

    m_pendingTimer[playerId] = m_timers.setPlayerTimeout(player, PUTDOWN_DURATION,
                                                         [this](IPlayer &p)
                                                         {
                                                             onPutdownFinished(p);
                                                         });
}

void HaulerJobSystem::creditParticipant(int playerId, std::int64_t amount, const std::string &popup,
                                        Milliseconds popupTime)
{
    // Кошелёк персистентный и per-account: начисляем write-through, попап — по факту.
    m_haulerWalletService.add(playerId, m_sessionService.getAccountId(playerId), amount);
    IPlayer *participant = m_core.getPlayers().get(playerId);
    if (!participant)
    {
        return; // напарник уже вне игры — начисление в БД всё равно ушло
    }
    m_screenNoticeService.show(
        *participant, fmt::format("{}, wallet ${}", popup, m_haulerWalletService.balanceOf(playerId)), popupTime,
        CREDIT_POPUP_COLOUR);
    m_audioService.playSound(*participant, PROGRESS_SOUND);
}

void HaulerJobSystem::onPutdownFinished(IPlayer &player)
{
    const int playerId = player.getID();
    // Хэндл сработавшего таймера гасим сразу: зачёт коробки может закрыть смену
    // (доставленный заказ, пропавший грузовик), а teardown отменяет m_pendingTimer —
    // тот самый таймер, из которого мы вызваны.
    if (playerId >= 0 && playerId < MAX_PLAYERS)
    {
        m_pendingTimer[playerId] = TimerService::Handle{};
    }
    const int owner = m_haulerJobService.shiftOwnerOf(playerId);
    if (owner < 0)
    {
        return;
    }
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(owner);
    if (phase != HaulerJobService::Phase::Loading && phase != HaulerJobService::Phase::Unloading)
    {
        return;
    }
    if (!m_haulerJobService.carryingOf(playerId))
    {
        return; // защитно (teardown/смерть сбросили несение)
    }

    IPlayer *driver = m_core.getPlayers().get(owner);
    if (!driver)
    {
        return; // водитель смены вне игры — его teardown разберёт состояние
    }
    const int partner = m_haulerJobService.partnerOf(owner);

    detachBox(player);
    m_animationService.stop(player);
    const int count = m_haulerJobService.finishCarry(playerId); // carrying=false, ++boxCount смены
    // Прогресс смены — окна простоя с нуля У ОБОИХ: в паре водитель ждёт по правилам
    // работы, и его счётчик должен двигать труд напарника.
    m_footIdleSeconds[playerId] = 0;
    m_driveIdleSeconds[playerId] = 0;
    m_footIdleSeconds[owner] = 0;
    m_driveIdleSeconds[owner] = 0;

    // Сколько коробок на плече: 10 в порту, остаток заказа в заказе (недовезённый
    // заказ возвращается в пул с прогрессом).
    const int target = legBoxes(owner);
    const bool orders = orderMode(owner);

    if (phase == HaulerJobService::Phase::Loading)
    {
        // Счётчик коробок — экранным попапом (не чат), English.
        if (orders)
        {
            // ПОГРУЗКА ЗАКАЗА НЕ ОПЛАЧИВАЕТСЯ. Она идёт на базе, в двух шагах от пикапа:
            // платная погрузка была бы фермой — взять заказ, загрузить кузов за деньги,
            // сорвать смену, взять заказ снова, никуда не выезжая. Заработок заказного
            // рейса весь на другом конце: прибытие, разгрузка и премия владельца.
            m_screenNoticeService.show(player, fmt::format("loaded {}/{}", count, target), COUNT_POPUP_TIME,
                                       NEUTRAL_POPUP_COLOUR);
            m_audioService.playSound(player, PROGRESS_SOUND);
        }
        else
        {
            // Плата за погрузку идёт ТОМУ, КТО НЁС: в паре это грузчик, соло — сам водитель.
            creditParticipant(playerId, PAY_PER_BOX_LOAD,
                              fmt::format("loaded {}/{} +${}", count, target, PAY_PER_BOX_LOAD), COUNT_POPUP_TIME);
        }

        if (count >= target)
        {
            player.sendClientMessage(INFO_COLOUR,
                                     u(orders ? fmt::format("Груз в кузове ({0}/{0}). Пора к точке", target)
                                              : fmt::format("Груз в кузове ({0}/{0}). Пора на базу", target)));
            if (partner >= 0)
            {
                driver->sendClientMessage(
                    INFO_COLOUR, u(orders ? fmt::format("Грузчик загрузил {0}/{0}. Везите заказ в точку", target)
                                          : fmt::format("Грузчик загрузил {0}/{0}. Возвращайтесь на базу", target)));
            }
            beginDriveBackPhase(*driver);
        }
        else
        {
            showBoxSource(player); // следующая коробка
        }
        return;
    }

    if (orders)
    {
        // Разгрузка заказа: коробка идёт НА СКЛАД ТОЧКИ, прогресс считает сам заказ (он
        // же его и персистит) — счётчик плеча тут вторичен. Заказ мог быть снят между
        // двумя коробками, поэтому платим ТОЛЬКО за зачёт в живой заказ: сперва
        // убеждаемся, что он есть, и лишь потом начисляем.
        if (!orderOfShift(owner))
        {
            dismissShiftOwner(owner, "Заказ снят — рейс отменён, заработок сохранён");
            return;
        }
        const int orderId = m_haulerJobService.orderIdOf(owner);
        // Заказ довезён, если зачёт был последним ЛИБО остатка уже нет: платить за
        // коробки в завершённый заказ (прогресс бы не двигался) нельзя.
        const bool finished = deliverOrderBox(owner) || m_orderService.boxesLeft(orderId) <= 0;
        const BusinessOrderService::Order *live = m_orderService.get(orderId);
        const int done = live ? live->boxesDone : BusinessOrderService::BOXES_PER_ORDER;
        creditParticipant(playerId, PAY_PER_BOX_UNLOAD,
                          fmt::format("delivered {}/{} +${}", done, BusinessOrderService::BOXES_PER_ORDER,
                                      PAY_PER_BOX_UNLOAD),
                          CREDIT_POPUP_TIME);
        if (finished)
        {
            completeOrder(*driver, player);
            return;
        }
        showBoxSource(player); // следующая коробка от зада грузовика
        return;
    }

    // Unloading: плата за КАЖДУЮ сданную коробку СРАЗУ в кошелёк (write-through) —
    // ТОМУ, КТО НЁС. Водитель в паре зарабатывает не на коробках, а на прибытии
    // (PAY_ARRIVAL за каждое плечо), поэтому доли обеих ролей сходятся.
    creditParticipant(playerId, PAY_PER_BOX_UNLOAD,
                      fmt::format("unloaded {}/{} +${}", count, HaulerJobService::BOXES_PER_LEG,
                                  PAY_PER_BOX_UNLOAD),
                      CREDIT_POPUP_TIME);

    if (count >= HaulerJobService::BOXES_PER_LEG)
    {
        // Полная разгрузка. Бонус — ТОЛЬКО паре, по PAIR_FULL_UNLOAD_BONUS каждому:
        // ради него грузовик и берут вдвоём.
        if (partner >= 0)
        {
            const std::string bonusPopup = fmt::format("pair bonus +${}", PAIR_FULL_UNLOAD_BONUS);
            creditParticipant(owner, PAIR_FULL_UNLOAD_BONUS, bonusPopup, BONUS_POPUP_TIME);
            creditParticipant(partner, PAIR_FULL_UNLOAD_BONUS, bonusPopup, BONUS_POPUP_TIME);
            const std::string done = fmt::format("Разгружено {0}/{0}! Бонус пары +${1} каждому",
                                                 HaulerJobService::BOXES_PER_LEG, PAIR_FULL_UNLOAD_BONUS);
            driver->sendClientMessage(INFO_COLOUR, u(done));
            player.sendClientMessage(INFO_COLOUR, u(done));
        }
        else
        {
            player.sendClientMessage(INFO_COLOUR,
                                     u(fmt::format("Разгружено {0}/{0}! Возьмите грузчика через /pair — за полный "
                                                   "рейс в паре доплачивают ${1} каждому",
                                                   HaulerJobService::BOXES_PER_LEG, PAIR_FULL_UNLOAD_BONUS)));
            m_screenNoticeService.show(player, "unload complete", BONUS_POPUP_TIME, CREDIT_POPUP_COLOUR);
        }
        finishRunPhase(*driver);
    }
    else
    {
        showBoxSource(player); // следующая коробка от зада грузовика
    }
}

void HaulerJobSystem::attachBox(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    detachBox(player); // защитно — не течь слотами, если что-то висело
    m_boxSlot[playerId] =
        m_attachmentService.attach(player, BOX_MODEL, PlayerBone_RightHand, BOX_OFFSET, BOX_ROTATION, BOX_SCALE);
    // slot может быть -1 (все 10 слотов заняты) — цикл продолжается без видимого
    // ящика; detachBox(-1) — безопасный no-op.
}

void HaulerJobSystem::detachBox(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    int &slot = m_boxSlot[playerId];
    if (slot >= 0)
    {
        m_attachmentService.detach(player, slot);
        slot = -1;
    }
}

// ------------------------------------------------------------------ таймер депо

void HaulerJobSystem::onDepotTick()
{
    releaseDepartedSpots(); // уехал с площадки — она свободна (O(SLOT_COUNT))
    pumpQueue();            // продвижение очереди на освободившиеся площадки
    tickActiveWorkers();    // окна активных работников (посадка/возврат/анти-AFK)
}

void HaulerJobSystem::releaseDepartedSpots()
{
    for (int i = 0; i < HaulerJobService::SLOT_COUNT; ++i)
    {
        const int holder = m_haulerJobService.holderOfSpot(i);
        if (holder < 0)
        {
            continue; // площадка и так свободна
        }
        // Площадку держит машина работника, пока физически стоит на ней. Уехал (или
        // машины не стало) — площадка идёт следующему из очереди. Сам работник при
        // этом остаётся со своим грузовиком: это НЕ конец смены.
        const int vid = m_haulerJobService.vehicleIdOf(holder);
        if (vid < 0 || !truckOnSpot(vid, i))
        {
            m_haulerJobService.releaseSpot(holder);
            // Выехал — окно выезда закрыто.
            m_reserveSeconds[holder] = 0;
            if (IPlayer *worker = m_core.getPlayers().get(holder))
            {
                m_screenTimerService.hide(*worker);
            }
        }
    }
}

bool HaulerJobSystem::spawnForWorker(IPlayer &player)
{
    const int playerId = player.getID();
    const int spot = m_haulerJobService.reservedSpotOf(playerId);
    if (spot < 0 || spot >= HaulerJobService::SLOT_COUNT)
    {
        return false;
    }
    IVehicle *truck = m_vehicleService.create(TRUCK_MODEL, SLOT_POS[spot], SLOT_ANGLE, -1, -1,
                                              VehicleService::Owner::Work, -1);
    if (!truck)
    {
        LogManager::log(Error, "HaulerJobSystem: vehicle pool full, truck not spawned");
        return false;
    }
    m_vehicleService.setInfiniteFuel(*truck, true); // рабочий транспорт — бак не расходуется
    m_haulerJobService.setStanding(spot, truck->getID());
    m_haulerJobService.setVehicle(playerId, truck->getID());
    return true;
}

void HaulerJobSystem::tickActiveWorkers()
{
    for (IPlayer *player : m_core.getPlayers().entries())
    {
        if (!player)
        {
            continue;
        }
        const int playerId = player->getID();
        if (playerId < 0 || playerId >= MAX_PLAYERS)
        {
            continue;
        }
        const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
        if (phase != HaulerJobService::Phase::NotWorking && phase != HaulerJobService::Phase::Queued)
        {
            tickWorker(*player);
        }
    }
}

void HaulerJobSystem::refreshUnloadSource(IPlayer &carrier)
{
    const int carrierId = carrier.getID();
    const int owner = m_haulerJobService.shiftOwnerOf(carrierId);
    if (owner < 0 || m_haulerJobService.carrierOf(owner) != carrierId)
    {
        return;
    }
    if (m_haulerJobService.phaseOf(owner) != HaulerJobService::Phase::Unloading ||
        m_haulerJobService.carryingOf(carrierId))
    {
        return;
    }

    // Цель «взять коробку» — зад ЖИВОГО грузовика. В кабине её нет вовсе (берут только
    // пешком), на выходе ставим от текущей позиции: фаза разгрузки стартует ещё на
    // подъезде, и точка иначе примерзает к месту зачёта последнего чекпоинта плеча.
    const bool inVehicle = m_stateService.getState(carrierId) != PlayerState_OnFoot;
    const bool shown = m_checkpointService.hasPersonal(carrierId);
    if (inVehicle && shown)
    {
        m_checkpointService.clearForPlayer(carrier);
    }
    else if (!inVehicle && !shown)
    {
        showBoxSource(carrier);
    }
}

void HaulerJobSystem::tickLoader(IPlayer &player)
{
    const int playerId = player.getID();
    const int owner = m_haulerJobService.partnerOf(playerId);
    if (owner < 0)
    {
        m_footIdleSeconds[playerId] = 0;
        return; // без пары грузчик просто ждёт приглашения — окон нет
    }

    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(owner);
    if (phase != HaulerJobService::Phase::Loading && phase != HaulerJobService::Phase::Unloading)
    {
        m_footIdleSeconds[playerId] = 0;
        return; // водитель в пути — грузчику работы нет, простой не его вина
    }

    refreshUnloadSource(player);

    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        m_footIdleSeconds[playerId] = 0;
        return; // едет в кабине к зоне вместе с водителем
    }

    // Анти-AFK переноски: прогресс — сданная коробка (сбрасывает счётчик обоим).
    const int idle = ++m_footIdleSeconds[playerId];
    if (idle == FOOT_NO_PROGRESS_WARN_SECONDS)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Нет прогресса по коробкам. Через {} секунд вас уволят за простой",
                                               FOOT_NO_PROGRESS_SECONDS - FOOT_NO_PROGRESS_WARN_SECONDS)));
    }
    else if (idle >= FOOT_NO_PROGRESS_SECONDS)
    {
        dismiss(player, "Вы уволены за простой на переноске коробок", ERROR_COLOUR);
    }
}

void HaulerJobSystem::tickWorker(IPlayer &player)
{
    const int playerId = player.getID();
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);

    if (phase == HaulerJobService::Phase::Standby)
    {
        tickLoader(player); // грузчик: своей фазы нет, живёт фазой напарника
        return;
    }

    if (phase == HaulerJobService::Phase::Reserved && drivingOwnTruck(playerId))
    {
        // Страховка: штатно фазу закрывает гейт руля в момент посадки, но за руль можно
        // попасть и мимо него (серверная посадка putInVehicle). Сверяемся с фактом.
        completeBoarding(player);
    }

    // Окно выезда идёт, пока за игроком числится ПЛОЩАДКА, а не пока длится фаза
    // Reserved: сесть за руль он мог уже секунду назад (маршрут открыт), но площадка
    // занята его грузовиком, и следующему из очереди её не отдать.
    if (m_haulerJobService.reservedSpotOf(playerId) >= 0)
    {
        const int elapsed = ++m_reserveSeconds[playerId];
        if (elapsed >= RESERVE_SECONDS)
        {
            failBoarding(player);
            return;
        }
        m_screenTimerService.show(player, RESERVE_SECONDS - elapsed, BOARDING_TIMER_LABEL);
        return; // маршрутные окна не ведём, пока не выехал
    }

    // Активная фаза держит ЛИЧНЫЙ грузовик: пропал (уничтожен/деспавн) -> увольнение штатно.
    const int vid = m_haulerJobService.vehicleIdOf(playerId);
    if (vid >= 0 && !m_vehicleService.get(vid))
    {
        dismiss(player, "Грузовик пропал — смена окончена", ERROR_COLOUR);
        return;
    }

    const bool driving = drivingOwnTruck(playerId);

    // Idle идёт по правилам фазы езды: грузовик за игроком, и место в кабине — его
    // рабочее место (цель рейса выбирают именно оттуда).
    if (phase == HaulerJobService::Phase::Idle || phase == HaulerJobService::Phase::DriveOut ||
        phase == HaulerJobService::Phase::DriveBack)
    {
        if (!driving)
        {
            // Вышел из грузовика в фазе езды — окно возврата за руль.
            const int away = ++m_exitSeconds[playerId];
            if (away >= RETURN_SECONDS)
            {
                dismiss(player, "Вы не вернулись за руль грузовика вовремя — вы уволены", ERROR_COLOUR);
                return;
            }
            m_screenTimerService.show(player, RETURN_SECONDS - away + 1, RETURN_TIMER_LABEL);
            return;
        }
        m_exitSeconds[playerId] = 0;
        m_screenTimerService.hide(player); // едет между чекпоинтами — активного окна нет
    }
    else
    {
        // Пешие фазы водителя: возврата за руль НЕ требуем (он легально пеший у грузовика).
        if (m_haulerJobService.partnerOf(playerId) >= 0)
        {
            // В паре коробки носит грузчик — ждать легально, окна переноски нет. Счётчик
            // простоя за рулём двигает труд напарника (сброс на каждой сданной коробке),
            // поэтому AFK-пара всё равно ограничена: молчащий грузчик отваливается по
            // своему окну, и водитель тут же возвращается к соло-правилам.
            if (!driving)
            {
                return;
            }
            m_footIdleSeconds[playerId] = 0;
        }
        else
        {
            refreshUnloadSource(player);
            if (!m_haulerJobService.isWorking(playerId))
            {
                return; // грузовик пропал — showBoxSource уволил
            }

            if (!driving)
            {
                // Анти-AFK переноски: прогресс — сданная коробка.
                const int idle = ++m_footIdleSeconds[playerId];
                if (idle == FOOT_NO_PROGRESS_WARN_SECONDS)
                {
                    player.sendClientMessage(
                        ERROR_COLOUR, u(fmt::format("Нет прогресса по коробкам. Через {} секунд вас уволят за простой",
                                                    FOOT_NO_PROGRESS_SECONDS - FOOT_NO_PROGRESS_WARN_SECONDS)));
                }
                else if (idle >= FOOT_NO_PROGRESS_SECONDS)
                {
                    dismiss(player, "Вы уволены за простой на переноске коробок", ERROR_COLOUR);
                    return;
                }
                return;
            }
            // За рулём в пешей фазе игрок ДОЕЗЖАЕТ до зоны погрузки/разгрузки (последний
            // чекпоинт плеча зачитывается до неё) или переставляет грузовик. Окно коробок в
            // это время не идёт — иначе доезд съедал бы время на переноску; вместо него
            // работает общий счётчик простоя за рулём, так что потолок остаётся.
            m_footIdleSeconds[playerId] = 0;
        }
    }

    // Анти-AFK за рулём: прогресс — зачтённый чекпоинт, сданная коробка либо (в Idle)
    // приезд на базу. Свой порог у фазы без цели: там прогресс — сам выбор рейса.
    const bool idlePhase = phase == HaulerJobService::Phase::Idle;
    const int limit = idlePhase ? NO_TARGET_SECONDS : DRIVE_NO_PROGRESS_SECONDS;
    const int warn = idlePhase ? NO_TARGET_WARN_SECONDS : DRIVE_NO_PROGRESS_WARN_SECONDS;
    const int idle = ++m_driveIdleSeconds[playerId];
    if (idle == warn)
    {
        player.sendClientMessage(
            ERROR_COLOUR,
            u(idlePhase ? fmt::format("Цель рейса так и не выбрана (/target). Через {} секунд вас уволят за простой",
                                      limit - warn)
                        : fmt::format("Нет прогресса по маршруту. Через {} секунд вас уволят за простой", limit - warn)));
    }
    else if (idle >= limit)
    {
        dismiss(player, idlePhase ? "Вы уволены за простой без выбранного рейса" : "Вы уволены за простой на маршруте",
                ERROR_COLOUR);
        return;
    }
}

// ------------------------------------------------------------------ увольнение

void HaulerJobSystem::failBoarding(IPlayer &player)
{
    const int playerId = player.getID();

    // Заказа у него быть не может: цель рейса выбирают уже за рулём, а он до руля не
    // добрался. Поэтому возврат в очередь ничего не «зажимает».
    m_checkpointService.clearRaceForPlayer(player);
    m_waypointService.clearFor(player); // снять маркер грузовика — не залипает в очереди
    m_screenTimerService.hide(player);
    const int vid = m_haulerJobService.vehicleIdOf(playerId);

    // Грузовик потерян — обслуживать грузчику нечего, пара распадается (сервис рвёт её
    // и сам, здесь — чтобы напарник узнал причину).
    splitPair(playerId, "Ваш водитель не успел сесть за руль — пара распалась");

    m_haulerJobService.requeueTail(playerId); // снять площадку + phase Queued + хвост очереди
    if (vid >= 0)
    {
        // Подавали ЕМУ — значит и убираем: pre-stock в депо нет, брошенный грузовик
        // просто занимал бы площадку и парк.
        m_vehicleService.destroy(vid);
    }
    clearCounters(playerId);

    player.sendClientMessage(
        ERROR_COLOUR, u("Вы не успели сесть за руль — грузовик снят. Вы возвращены в конец очереди"));
    m_screenNoticeService.show(player, "boarding failed - back to queue", FAIL_POPUP_TIME, ERROR_POPUP_COLOUR);
}

void HaulerJobSystem::dismiss(IPlayer &player, const std::string &reason, const Colour &colour)
{
    if (!m_haulerJobService.isWorking(player.getID()))
    {
        return; // защитно
    }
    teardownShift(player);
    player.sendClientMessage(colour, u(reason));
}

void HaulerJobSystem::dismissShiftOwner(int ownerId, const std::string &reason)
{
    if (IPlayer *driver = m_core.getPlayers().get(ownerId))
    {
        dismiss(*driver, reason, ERROR_COLOUR);
    }
}

void HaulerJobSystem::teardownShift(IPlayer &player)
{
    const int playerId = player.getID();

    // Пара не переживает конец смены ни одной из сторон: ушёл грузчик — водитель
    // снова соло и снова носит сам; ушёл водитель — грузчику нечего обслуживать.
    // Оба узнают об этом; напарник остаётся работать (роль и наём не теряются).
    splitPair(playerId, m_haulerJobService.roleOf(playerId) == HaulerJobService::Role::Loader
                            ? "Ваш грузчик выбыл — дальше вы работаете один"
                            : "Ваш водитель закончил смену — пара распалась, ждите нового приглашения");

    if (playerId >= 0 && playerId < MAX_PLAYERS)
    {
        m_timers.cancel(m_pendingTimer[playerId]); // стале-таймер подъёма/укладки не продвинет ничего
    }
    m_checkpointService.clearRaceForPlayer(player);
    m_checkpointService.clearForPlayer(player);
    m_waypointService.clearFor(player);
    m_screenTimerService.hide(player);
    detachBox(player);                         // снять коробку, если несли
    m_animationService.stop(player);           // no-op, если не играла
    m_stateService.clearSpecialAction(player); // снять carry, если несли

    const int vid = m_haulerJobService.vehicleIdOf(playerId);

    // Заказ смены (если рейс был заказной) возвращается в пул КАК ЕСТЬ, с уже
    // засчитанными коробками. Обязательно ДО endShift: он стирает состояние вместе с
    // привязкой к заказу, и заказ остался бы забронированным навсегда.
    releaseOrderOf(playerId);

    // Грузовик всегда деспавним: он личный, подавался этому работнику. Оставлять его
    // на площадке было бы возвратом к pre-stock — машина стояла бы ничья и занимала
    // и площадку, и место в парке.
    m_haulerJobService.endShift(playerId);
    if (vid >= 0)
    {
        m_vehicleService.destroy(vid);
    }
    clearCounters(playerId);
    m_navLockService.release(playerId); // конец смены — снять лок навигации
}

// ------------------------------------------------------------------ лайфцикл сессии

void HaulerJobSystem::onSessionEnd(IPlayer &player)
{
    const int playerId = player.getID();

    if (m_haulerJobService.isWorking(playerId))
    {
        const bool wasQueued = m_haulerJobService.phaseOf(playerId) == HaulerJobService::Phase::Queued;
        teardownShift(player); // без сообщения — игрок уходит
        if (wasQueued)
        {
            notifyQueueShift();
        }
    }
    m_haulerWalletService.reset(playerId); // teardown ТОЛЬКО кэша — баланс остаётся в БД
}

void HaulerJobSystem::onPlayerDeath(IPlayer &player)
{
    if (!m_haulerJobService.isWorking(player.getID()))
    {
        return; // не в смене — смерть работы не касается
    }
    dismiss(player, "Вы погибли — смена развозчика завершена", ERROR_COLOUR);
}

void HaulerJobSystem::loadWallet(IPlayer &player, const PlayerSessionService::Session &session)
{
    DatabaseManager::selectQuery<std::int64_t>(
        [accountId = session.accountId](mysqlx::Schema schema) -> std::int64_t
        {
            mysqlx::RowResult result = schema.getTable("hauler_wallet")
                                           .select("balance")
                                           .where("account_id = :account")
                                           .limit(1)
                                           .bind("account", accountId)
                                           .execute();
            if (mysqlx::Row row = result.fetchOne())
            {
                return row.get(0).get<std::int64_t>();
            }
            return std::int64_t{0}; // нет строки -> кошелёк пуст
        },
        [this, playerId = player.getID(), serial = session.serial](std::int64_t balance)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
            {
                return;
            }
            m_haulerWalletService.load(playerId, balance);
            m_jobWalletService.notifyLoaded(playerId); // общий список знает, что баланс готов
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "HaulerJobSystem: failed to load hauler wallet: " + error);
        });
}

// ------------------------------------------------------------------ helpers

bool HaulerJobSystem::drivingOwnTruck(int playerId) const
{
    const int vid = m_haulerJobService.vehicleIdOf(playerId);
    return vid >= 0 && m_vehicleService.getDriver(vid) == playerId;
}

bool HaulerJobSystem::truckOnSpot(int vid, int spot) const
{
    if (vid < 0 || spot < 0 || spot >= HaulerJobService::SLOT_COUNT)
    {
        return false;
    }
    IVehicle *truck = m_vehicleService.get(vid);
    if (!truck)
    {
        return false;
    }
    return distanceSq2D(truck->getPosition(), SLOT_POS[spot]) <= SPOT_RADIUS * SPOT_RADIUS;
}

bool HaulerJobSystem::spotClear(int spot) const
{
    if (spot < 0 || spot >= HaulerJobService::SLOT_COUNT)
    {
        return false; // вне диапазона -> считаем занятой (не спавним)
    }
    // Многопробная занятость (центр + вперёд/назад по SLOT_ANGLE), как у автобуса/
    // ParkingSystem: длинный грузовик детектится телом, соседи (⊥ пробам) ложно не заняты.
    const Vector3 &c = SLOT_POS[spot];
    const Vector3 forward = Geometry::forwardOf(c, SLOT_ANGLE, SPOT_PROBE_OFFSET);
    const Vector3 back = Geometry::backOf(c, SLOT_ANGLE, SPOT_PROBE_OFFSET);
    return !m_vehicleService.anyVehicleNear(c, SPOT_OCCUPIED_RADIUS) &&
           !m_vehicleService.anyVehicleNear(forward, SPOT_OCCUPIED_RADIUS) &&
           !m_vehicleService.anyVehicleNear(back, SPOT_OCCUPIED_RADIUS);
}

bool HaulerJobSystem::backOfTruck(int shiftOwnerId, Vector3 &out) const
{
    // Грузовик принадлежит ВОДИТЕЛЮ смены: у грузчика vehicleId всегда -1.
    const int vid = m_haulerJobService.vehicleIdOf(shiftOwnerId);
    IVehicle *truck = m_vehicleService.get(vid);
    if (!truck)
    {
        return false; // грузовик пропал — вызывающий увольняет
    }
    out = Geometry::backOf(truck->getPosition(), truck->getZAngle(), TRUCK_REAR_OFFSET);
    return true;
}

Vector3 HaulerJobSystem::randomWarehousePoint(const Vector3 &awayFrom)
{
    // Единый источник координат склада (общий с портом) — PortJobService::dropPositions.
    // Выбираем только из точек не ближе MIN_CARRY_DISTANCE к кузову: точки разнесены
    // на ~90 м, поэтому пустым отбор не бывает, а гарантия «коробку надо нести» есть.
    const std::array<Vector3, PortJobService::DROP_COUNT> &pts = PortJobService::dropPositions();
    constexpr float minSq = MIN_CARRY_DISTANCE * MIN_CARRY_DISTANCE;

    // Имя candidates, а не far: far — легаси-макрос windows.h, разворачивается в пустоту.
    std::array<int, PortJobService::DROP_COUNT> candidates{};
    int count = 0;
    int farthest = 0;
    float farthestSq = -1.0f;
    for (int i = 0; i < PortJobService::DROP_COUNT; ++i)
    {
        const float distSq = distanceSq2D(pts[i], awayFrom);
        if (distSq >= minSq)
        {
            candidates[count++] = i;
        }
        if (distSq > farthestSq)
        {
            farthestSq = distSq;
            farthest = i;
        }
    }
    if (count == 0)
    {
        return pts[farthest]; // защитно: грузовик посреди всех точек — берём дальнюю
    }

    std::uniform_int_distribution<int> dist(0, count - 1);
    return pts[candidates[dist(m_rng)]];
}

void HaulerJobSystem::clearCounters(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_reserveSeconds[playerId] = 0;
    m_exitSeconds[playerId] = 0;
    m_driveIdleSeconds[playerId] = 0;
    m_footIdleSeconds[playerId] = 0;
}
