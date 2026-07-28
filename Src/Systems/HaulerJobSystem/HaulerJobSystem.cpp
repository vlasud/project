#include "Systems/HaulerJobSystem/HaulerJobSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/Geometry/Geometry.h"
#include <cstdint>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <string>

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
constexpr std::int64_t HAULER_JOB_ENTRY_FEE = 1000; // невозвратный взнос наличными при устройстве ВОДИТЕЛЕМ
// $ за КАЖДУЮ РАЗГРУЖЕННУЮ коробку — КАЖДОМУ участнику смены (соло-водитель получает
// столько же, пара — по столько же каждый). Погрузка не оплачивается.
constexpr std::int64_t PAY_PER_BOX_UNLOAD = 500;
// Бонус за полную разгрузку 10/10 — КАЖДОМУ, и ТОЛЬКО в паре: это рычаг, ради которого
// пару собирают (один грузовик — два рабочих места).
constexpr std::int64_t PAIR_FULL_UNLOAD_BONUS = 500;
constexpr int RESERVE_SECONDS = 30;                 // окно посадки (Reserved)
constexpr int RETURN_SECONDS = 30;                  // окно возврата за руль — ТОЛЬКО фазы езды
// Анти-AFK езды: за рулём без единого зачёта чекпоинта -> увольнение (порог выше
// любого легального межчекпоинтного отрезка), с предупреждением за 30 с.
constexpr int DRIVE_NO_PROGRESS_SECONDS = 240;
constexpr int DRIVE_NO_PROGRESS_WARN_SECONDS = 210;
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

// Маршрут ЕЗДА-ТУДА: 13 замеров. Прибытие-точки нет — зачёт последнего чекпоинта (13)
// сразу запускает ПОГРУЗКУ; его стрелка ведёт на склад-зону, парковка свободная.
const Vector3 ROUTE_OUT[HaulerJobService::OUT_LENGTH] = {
    {2218.5100f, -2235.9246f, 13.7202f}, // 1 первая точка после посадки за руль
    {2264.8379f, -2234.8633f, 13.6613f}, // 2
    {2283.6428f, -2254.0869f, 13.5273f}, // 3
    {2227.2112f, -2322.4084f, 13.5487f}, // 4
    {2159.9729f, -2428.3887f, 13.5483f}, // 5
    {2167.3706f, -2494.2815f, 13.5479f}, // 6
    {2213.3196f, -2497.2854f, 13.5832f}, // 7
    {2222.2620f, -2566.3025f, 13.5687f}, // 8
    {2229.4343f, -2658.6750f, 13.5619f}, // 9
    {2353.4309f, -2665.9319f, 13.6710f}, // 10
    {2478.1963f, -2660.3931f, 13.6818f}, // 11
    {2487.6196f, -2521.7490f, 13.6805f}, // 12
    {2662.2156f, -2506.6082f, 13.6654f}, // 13 последний -> старт погрузки
};

// Маршрут ЕЗДА-ОБРАТНО: 8 точек. Первая — выездная дорога ИЗ зоны склада (бывшая
// точка 14 плеча «туда»); финиш-точки нет — зачёт последней (8) сразу запускает
// РАЗГРУЗКУ, её стрелка ведёт на точку выгрузки базы, парковка свободная.
//
// Последняя точка — ТА ЖЕ координата, что ROUTE_OUT[0] (первая точка плеча «туда»): плечо
// «обратно» замыкается там, где начинается «туда». Прежде плечо кончалось на точке 7,
// в 97 м от точки выгрузки — фаза разгрузки стартовала посреди дороги, до базы игрока
// уже ничто не вело (race-чекпоинт со стрелкой снимается зачётом). Теперь неразмеченный
// хвост 46 м, и он лежит внутри депо, где цель видна.
const Vector3 ROUTE_BACK[HaulerJobService::BACK_LENGTH] = {
    {2740.1736f, -2403.9192f, 13.6343f}, // 1 выезд из зоны склада
    {2619.6836f, -2402.5168f, 13.6672f}, // 2
    {2528.4968f, -2321.4109f, 23.3411f}, // 3
    {2352.1296f, -2145.5469f, 18.0327f}, // 4
    {2293.9658f, -2087.4048f, 13.5040f}, // 5
    {2237.1128f, -2123.3354f, 13.5015f}, // 6
    {2204.1184f, -2156.0852f, 13.5616f}, // 7
    {2218.5100f, -2235.9246f, 13.7202f}, // 8 последний -> старт разгрузки (= ROUTE_OUT[0])
};

// Плечо езды для фазы (DriveOut -> туда, DriveBack -> обратно); прочие фазы — нет
// плеча (nullptr). В Reserved маршрута ещё нет: до руля цель одна — свой грузовик.
const Vector3 *routeForPhase(HaulerJobService::Phase phase, int &lenOut)
{
    if (phase == HaulerJobService::Phase::DriveOut)
    {
        lenOut = HaulerJobService::OUT_LENGTH;
        return ROUTE_OUT;
    }
    if (phase == HaulerJobService::Phase::DriveBack)
    {
        lenOut = HaulerJobService::BACK_LENGTH;
        return ROUTE_BACK;
    }
    lenOut = 0;
    return nullptr;
}

// Куда указывает стрелка ПОСЛЕДНЕГО чекпоинта плеча — ЦЕЛЬ пешей фазы: точка выгрузки
// базы («обратно») или центр склад-зоны порта («туда»). Доводит к зоне, где грузовик
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
      m_attachmentService(serviceRegister.getService<AttachmentService>()), m_rng(std::random_device{}())
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
        // Одна строка на обе роли: у грузчика ни грузовика, ни взноса не было.
        fmt::format("Пара распадётся. Грузовик, если он за вами, будет снят, а взнос ${} (его платит только "
                    "водитель) не возвращается — устройство заново снова платное. Заработок в кошельке "
                    "развозчика сохранится, его можно забрать у пикапа работы.",
                    HAULER_JOB_ENTRY_FEE),
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

    m_mapIconService.addGlobal(JOB_MAP_ICON, EMPLOY_PICKUP_POS, Colour::White(), MapIconStyle_Global);

    // Заспавнить pre-stock грузовики сразу на старте (площадки чисты); дальше их
    // поддерживает тик депо.
    restockDepot();

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
    const std::string body = fmt::format("Водитель\tсвой грузовик, взнос ${}\nГрузчик\tбез грузовика и без взноса, "
                                         "работает в паре с водителем",
                                         HAULER_JOB_ENTRY_FEE);
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
    const int playerId = player.getID();

    if (!m_sessionService.isActive(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы работать"));
        return;
    }
    // Клиенту не доверяем: гейт на СЕРВЕРНОМ стейте.
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из транспорта, чтобы устроиться развозчиком"));
        return;
    }
    if (m_haulerJobService.isWorking(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы уже работаете развозчиком"));
        return;
    }
    // Взаимное исключение работ: чекпоинт-слот и лок навигации принадлежат ТЕКУЩЕЙ
    // смене. isWorking(развозчик) выше уже ложно, значит держатель лока — ДРУГАЯ
    // работа. Гейтим ДО списания взноса.
    if (m_navLockService.isLocked(playerId))
    {
        player.sendClientMessage(
            ERROR_COLOUR, u(fmt::format("Нельзя устроиться развозчиком: {}", m_navLockService.lockReason(playerId))));
        return;
    }

    // Невозвратный вступительный взнос НАЛИЧНЫМИ: проверка + списание атомарно и ДО
    // перевода в работники. Взнос — денежный сток, назад не возвращается.
    const unsigned long long cash = m_moneyService.getMoney(playerId);
    if (!m_moneyService.take(player, static_cast<unsigned long long>(HAULER_JOB_ENTRY_FEE)))
    {
        player.sendClientMessage(
            ERROR_COLOUR,
            u(fmt::format("Недостаточно наличных: вступительный взнос ${} (у вас ${})", HAULER_JOB_ENTRY_FEE, cash)));
        return;
    }
    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Вступительный взнос ${} списан (не возвращается)", HAULER_JOB_ENTRY_FEE)));

    const HaulerJobService::StartOutcome outcome = m_haulerJobService.startWork(playerId);
    // Лок навигации на ВСЮ смену; освобождается на любом её конце.
    m_navLockService.acquire(playerId, "идёт смена портового развозчика");
    // Предзагрузка либы CARRY заранее (первая укладка/подъём иначе не проиграется).
    m_animationService.preloadLibrary(player, "CARRY");

    if (outcome.result == HaulerJobService::StartResult::Queued)
    {
        player.sendClientMessage(
            INFO_COLOUR,
            u(fmt::format("Свободных грузовиков в депо сейчас нет. Вы в очереди, место {}", outcome.queuePosition)));
        return;
    }
    // Reserved — свободный стоящий грузовик закреплён за игроком.
    onReserved(player);
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
        m_haulerJobService.endShift(playerId, false); // грузовика нет — busKept неважен
        clearCounters(playerId);
        m_navLockService.release(playerId);
        player.sendClientMessage(INFO_COLOUR, u("Вы вышли из очереди на грузовик"));
        notifyQueueShift();
        return;
    }
    // Активная смена — штатное увольнение (кошелёк остаётся).
    dismiss(player, "Смена окончена. Заработок сохранён в кошельке развозчика — заберите через «Забрать деньги»",
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
                   fmt::format("Водитель {}[{}] зовёт вас грузчиком в свой рейс.\n\nВы будете носить коробки, он — "
                               "водить. Каждому ${} за коробку и ${} сверху за полный рейс в паре.",
                               driverSafe, driverId, PAY_PER_BOX_UNLOAD, PAIR_FULL_UNLOAD_BONUS),
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
    body += fmt::format("Вступительный взнос\t${} наличными, невозвратный; грузчик не платит\n",
                        HAULER_JOB_ENTRY_FEE);
    body += fmt::format("Ставка\t${} за разгруженную коробку КАЖДОМУ участнику (погрузка не оплачивается)\n",
                        PAY_PER_BOX_UNLOAD);
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
    case HaulerJobService::Phase::DriveOut:
        status = fmt::format("едет в порт, чекпоинт {}/{}", m_haulerJobService.driveIndexOf(playerId) + 1,
                             HaulerJobService::OUT_LENGTH);
        break;
    case HaulerJobService::Phase::Loading:
        status = fmt::format("грузит на складе, коробок {}/{}", m_haulerJobService.boxCountOf(playerId),
                             HaulerJobService::BOXES_PER_LEG);
        break;
    case HaulerJobService::Phase::DriveBack:
        status = fmt::format("едет на базу, чекпоинт {}/{}", m_haulerJobService.driveIndexOf(playerId) + 1,
                             HaulerJobService::BACK_LENGTH);
        break;
    case HaulerJobService::Phase::Unloading:
        status = fmt::format("разгружает на базе, коробок {}/{}", m_haulerJobService.boxCountOf(playerId),
                             HaulerJobService::BOXES_PER_LEG);
        break;
    }
    body += fmt::format("Ваш статус\t{}\n", status);

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
    // Свободный стоящий pre-stock грузовик депо: не пускает НИКОГО (ждёт резерва).
    if (m_haulerJobService.isFreeStandingVehicle(vid))
    {
        if (m_haulerJobService.phaseOf(playerId) == HaulerJobService::Phase::Reserved)
        {
            player.sendClientMessage(ERROR_COLOUR,
                                     u("Ваш грузовик отмечен красным маркером — садитесь только в него"));
        }
        else
        {
            player.sendClientMessage(ERROR_COLOUR, u("Это грузовик депо — оформите смену через «Начать работу»"));
        }
        return false;
    }
    return true; // не наш грузовик — гейт не наш
}

// ------------------------------------------------------------------ резерв/очередь

void HaulerJobSystem::onReserved(IPlayer &player)
{
    const int playerId = player.getID();
    clearCounters(playerId); // свежее окно посадки

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
        u(fmt::format("За вами закреплён грузовик на площадке — он отмечен маркером. У вас {} секунд сесть за руль",
                      RESERVE_SECONDS)));
    m_screenNoticeService.show(player, "truck reserved - get in", RESERVE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
}

void HaulerJobSystem::completeBoarding(IPlayer &player)
{
    const int playerId = player.getID();
    m_haulerJobService.completeBoarding(playerId); // Reserved -> DriveOut, площадка освобождается
    clearCounters(playerId);                       // окно посадки закрыто
    m_waypointService.clearFor(player);            // маркер грузовика больше не нужен
    m_screenTimerService.hide(player);
    showDriveCheckpoint(player); // теперь маршрут: плечо OUT, индекс 0
    player.sendClientMessage(INFO_COLOUR, u("Вы за рулём — следуйте по чекпоинтам в порт"));
}

void HaulerJobSystem::pumpQueue()
{
    bool promoted = false;
    for (;;)
    {
        const HaulerJobService::Promotion promotion = m_haulerJobService.promoteQueue();
        if (promotion.playerId < 0)
        {
            break; // очередь пуста / свободных стоящих грузовиков нет
        }
        IPlayer *next = m_core.getPlayers().get(promotion.playerId);
        if (!next)
        {
            // Продвинутый работник пропал: снять резерв (грузовик остаётся pre-stock).
            m_haulerJobService.endShift(promotion.playerId, true);
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
    int len = 0;
    const Vector3 *route = routeForPhase(phase, len);
    if (!route)
    {
        return; // защитно (не фаза езды)
    }
    const int index = m_haulerJobService.driveIndexOf(playerId);
    if (index < 0 || index >= len)
    {
        return;
    }
    // Все маршрутные точки — race move (RACE_NORMAL со стрелкой). Обычная точка -> стрелка
    // на следующую точку плеча; ПОСЛЕДНЯЯ -> стрелка на ЦЕЛЬ пешей фазы (склад-зона / точка
    // выгрузки). Зачёт последней точки запускает погрузку/разгрузку; парковку грузовика
    // игрок выбирает сам (чекпоинт зада пересчитывается от живой позиции).
    const Vector3 &next = (index == len - 1) ? phaseTarget(phase) : route[index + 1];
    m_checkpointService.setRaceForPlayer(player, RaceCheckpointType::RACE_NORMAL, route[index], next, MOVE_CP_RADIUS,
                                         [this](IPlayer &p)
                                         {
                                             onDriveCheckpointEnter(p);
                                         });
}

void HaulerJobSystem::onDriveCheckpointEnter(IPlayer &player)
{
    const int playerId = player.getID();
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
    if (phase != HaulerJobService::Phase::DriveOut && phase != HaulerJobService::Phase::DriveBack)
    {
        return; // не в фазе езды — событие не наше (в Reserved маршрута ещё нет)
    }
    // Зачёт ТОЛЬКО за рулём СВОЕГО грузовика (серверный getDriver, не заявление клиента).
    if (!drivingOwnTruck(playerId))
    {
        return;
    }

    int len = 0;
    const Vector3 *route = routeForPhase(phase, len);
    if (!route)
    {
        return;
    }
    const int index = m_haulerJobService.driveIndexOf(playerId);
    if (index >= len - 1)
    {
        // Достигнута последняя точка плеча — переход в пешую фазу.
        if (phase == HaulerJobService::Phase::DriveOut)
        {
            beginLoadingPhase(player);
        }
        else
        {
            beginUnloadingPhase(player);
        }
        return;
    }
    m_haulerJobService.advanceDrive(playerId);
    m_driveIdleSeconds[playerId] = 0; // есть прогресс — окно простоя с нуля
    showDriveCheckpoint(player);
}

void HaulerJobSystem::beginLoadingPhase(IPlayer &player)
{
    const int playerId = player.getID();
    m_haulerJobService.beginLoading(playerId); // DriveOut -> Loading, boxCount=0
    m_checkpointService.clearRaceForPlayer(player);
    m_screenTimerService.hide(player); // в пешей фазе окна езды нет
    m_footIdleSeconds[playerId] = 0;
    player.sendClientMessage(
        INFO_COLOUR,
        u(fmt::format("Вы прибыли в порт. Загрузите {} коробок со склада в грузовик", HaulerJobService::BOXES_PER_LEG)));
    m_screenNoticeService.show(player, fmt::format("port: load {} boxes into truck", HaulerJobService::BOXES_PER_LEG),
                               PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);

    // Коробки грузит НОСИЛЬЩИК смены: соло-водитель сам, в паре — грузчик.
    const int carrierId = m_haulerJobService.carrierOf(playerId);
    if (IPlayer *carrier = m_core.getPlayers().get(carrierId))
    {
        if (carrierId != playerId)
        {
            carrier->sendClientMessage(INFO_COLOUR,
                                       u(fmt::format("Грузовик в порту. Загрузите {} коробок со склада",
                                                     HaulerJobService::BOXES_PER_LEG)));
            m_screenNoticeService.show(*carrier, "port: load the truck", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
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
    m_screenNoticeService.show(player, "drive back to base", PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
    showDriveCheckpoint(player); // плечо BACK, индекс 0 (выезд из зоны склада)
}

void HaulerJobSystem::beginUnloadingPhase(IPlayer &player)
{
    const int playerId = player.getID();
    m_haulerJobService.beginUnloading(playerId); // DriveBack -> Unloading, boxCount=0
    m_checkpointService.clearRaceForPlayer(player);
    m_screenTimerService.hide(player);
    m_footIdleSeconds[playerId] = 0;
    player.sendClientMessage(
        INFO_COLOUR,
        u(fmt::format("Вы на базе. Разгрузите {} коробок на точку выгрузки", HaulerJobService::BOXES_PER_LEG)));
    m_screenNoticeService.show(player, fmt::format("base: unload {} boxes", HaulerJobService::BOXES_PER_LEG),
                               PHASE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
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

void HaulerJobSystem::completeCyclePhase(IPlayer &player)
{
    const int playerId = player.getID();
    m_haulerJobService.completeCycle(playerId); // Unloading -> DriveOut, driveIndex=0, boxCount=0
    m_checkpointService.clearForPlayer(player);
    m_driveIdleSeconds[playerId] = 0;
    m_exitSeconds[playerId] = 0;
    player.sendClientMessage(INFO_COLOUR, u("Новый рейс! Езжайте в порт за грузом"));
    showDriveCheckpoint(player); // плечо OUT, индекс 0 (точка 1)
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
    // и подогнав грузовик вплотную к точке выгрузки он стоял бы сразу в обоих чекпоинтах.
    // Коробку не выдаём: руки свободны, отогнать грузовик можно сразу (на погрузке то же
    // правило решается выбором дальней точки склада — см. randomWarehousePoint).
    Vector3 rear;
    if (m_haulerJobService.phaseOf(owner) == HaulerJobService::Phase::Unloading && backOfTruck(owner, rear) &&
        distanceSq2D(rear, BASE_UNLOAD_POS) < MIN_CARRY_DISTANCE * MIN_CARRY_DISTANCE)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Грузовик стоит вплотную к точке выгрузки — отгоните его на {} м",
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
}

void HaulerJobSystem::onPutdownFinished(IPlayer &player)
{
    const int playerId = player.getID();
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

    if (phase == HaulerJobService::Phase::Loading)
    {
        if (count >= HaulerJobService::BOXES_PER_LEG)
        {
            player.sendClientMessage(
                INFO_COLOUR, u(fmt::format("Груз в кузове ({0}/{0}). Пора на базу", HaulerJobService::BOXES_PER_LEG)));
            if (partner >= 0)
            {
                driver->sendClientMessage(INFO_COLOUR,
                                          u(fmt::format("Грузчик загрузил {0}/{0}. Возвращайтесь на базу",
                                                        HaulerJobService::BOXES_PER_LEG)));
            }
            beginDriveBackPhase(*driver);
        }
        else
        {
            // Счётчик коробок — экранным попапом (не чат), English.
            m_screenNoticeService.show(player, fmt::format("loaded {}/{}", count, HaulerJobService::BOXES_PER_LEG),
                                       COUNT_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
            showBoxSource(player); // следующая коробка
        }
        return;
    }

    // Unloading: плата за КАЖДУЮ сданную коробку СРАЗУ в кошелёк (write-through) —
    // КАЖДОМУ участнику смены. Соло-водитель получает ту же ставку.
    const std::string boxPopup = fmt::format("+${}", PAY_PER_BOX_UNLOAD);
    creditParticipant(owner, PAY_PER_BOX_UNLOAD, boxPopup, CREDIT_POPUP_TIME);
    if (partner >= 0)
    {
        creditParticipant(partner, PAY_PER_BOX_UNLOAD, boxPopup, CREDIT_POPUP_TIME);
    }

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
        completeCyclePhase(*driver);
    }
    else
    {
        // Счётчик коробок — экранным попапом (не чат), English; выплата остаётся
        // отдельным credit-попапом выше.
        m_screenNoticeService.show(player, fmt::format("unloaded {}/{}", count, HaulerJobService::BOXES_PER_LEG),
                                   COUNT_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
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
    restockDepot();      // реконсиляция + пере-сток пустых чистых площадок (O(SLOT_COUNT))
    pumpQueue();         // продвижение очереди на освободившиеся стоящие грузовики
    tickActiveWorkers(); // окна активных работников (посадка/возврат/анти-AFK)
}

void HaulerJobSystem::restockDepot()
{
    for (int i = 0; i < HaulerJobService::SLOT_COUNT; ++i)
    {
        const int vid = m_haulerJobService.standingVehicle(i);
        if (vid >= 0)
        {
            if (!truckOnSpot(vid, i))
            {
                // Грузовик покинул площадку/исчез: резервный увёл работник — оставляем
                // (стейт разрулит), свободный pre-stock сдвинут тараном — деспавним.
                const bool reserved = m_haulerJobService.spotReserved(i);
                m_haulerJobService.clearStanding(i);
                if (!reserved)
                {
                    m_vehicleService.destroy(vid);
                }
            }
            continue; // ещё стоит — не пере-стокуем
        }
        // Потолок парка: упёрлись — площадку не пере-стокуем. Свободных стоящих
        // грузовиков не появляется, и новые работники штатно уходят в FIFO-очередь;
        // освободится машина (увольнение/выход) — пере-сток возобновится сам.
        if (m_haulerJobService.truckCount() >= HaulerJobService::MAX_TRUCKS)
        {
            continue;
        }
        if (spotClear(i))
        {
            spawnPrestock(i);
        }
    }
}

void HaulerJobSystem::spawnPrestock(int spot)
{
    if (spot < 0 || spot >= HaulerJobService::SLOT_COUNT)
    {
        return;
    }
    IVehicle *truck = m_vehicleService.create(TRUCK_MODEL, SLOT_POS[spot], SLOT_ANGLE, -1, -1,
                                              VehicleService::Owner::Work, -1);
    if (!truck)
    {
        LogManager::log(Error, "HaulerJobSystem: vehicle pool full, prestock truck not spawned");
        return;
    }
    m_vehicleService.setInfiniteFuel(*truck, true); // рабочий транспорт — бак не расходуется
    m_haulerJobService.setStanding(spot, truck->getID());
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

    if (phase == HaulerJobService::Phase::Reserved)
    {
        // Страховка: штатно окно закрывает гейт руля в момент посадки, но за руль можно
        // попасть и мимо него (серверная посадка putInVehicle). Сверяемся с фактом.
        if (drivingOwnTruck(playerId))
        {
            completeBoarding(player);
            return;
        }
        // Окно посадки: не сел за руль своего грузовика за RESERVE_SECONDS.
        const int elapsed = ++m_reserveSeconds[playerId];
        if (elapsed >= RESERVE_SECONDS)
        {
            failBoarding(player);
            return;
        }
        m_screenTimerService.show(player, RESERVE_SECONDS - elapsed, BOARDING_TIMER_LABEL);
        return;
    }

    // Активная фаза держит ЛИЧНЫЙ грузовик: пропал (уничтожен/деспавн) -> увольнение штатно.
    const int vid = m_haulerJobService.vehicleIdOf(playerId);
    if (vid >= 0 && !m_vehicleService.get(vid))
    {
        dismiss(player, "Грузовик пропал — смена окончена", ERROR_COLOUR);
        return;
    }

    const bool driving = drivingOwnTruck(playerId);

    if (phase == HaulerJobService::Phase::DriveOut || phase == HaulerJobService::Phase::DriveBack)
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

    // Анти-AFK за рулём: нет прогресса (зачтённых чекпоинтов / сданных коробок).
    const int idle = ++m_driveIdleSeconds[playerId];
    if (idle == DRIVE_NO_PROGRESS_WARN_SECONDS)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Нет прогресса по маршруту. Через {} секунд вас уволят за простой",
                                               DRIVE_NO_PROGRESS_SECONDS - DRIVE_NO_PROGRESS_WARN_SECONDS)));
    }
    else if (idle >= DRIVE_NO_PROGRESS_SECONDS)
    {
        dismiss(player, "Вы уволены за простой на маршруте", ERROR_COLOUR);
        return;
    }
}

// ------------------------------------------------------------------ увольнение

void HaulerJobSystem::failBoarding(IPlayer &player)
{
    const int playerId = player.getID();

    m_checkpointService.clearRaceForPlayer(player);
    m_waypointService.clearFor(player); // снять маркер грузовика — не залипает в очереди
    m_screenTimerService.hide(player);
    const int spot = m_haulerJobService.reservedSpotOf(playerId);
    const int vid = m_haulerJobService.vehicleIdOf(playerId);
    const bool onSpot = truckOnSpot(vid, spot);

    // Грузовик потерян — обслуживать грузчику нечего, пара распадается (сервис рвёт её
    // и сам, здесь — чтобы напарник узнал причину).
    splitPair(playerId, "Ваш водитель не успел сесть за руль — пара распалась");

    m_haulerJobService.requeueTail(playerId, onSpot); // снять резерв + phase Queued + хвост очереди
    if (!onSpot && vid >= 0)
    {
        m_vehicleService.destroy(vid); // уведён с площадки -> деспавн (хлам не бросаем)
    }
    clearCounters(playerId);

    if (onSpot)
    {
        player.sendClientMessage(
            ERROR_COLOUR,
            u("Вы не успели сесть за руль — грузовик остался на площадке. Вы возвращены в конец очереди"));
    }
    else
    {
        // За руль не сели, но грузовик уже не на площадке — его столкнули/утащили.
        player.sendClientMessage(
            ERROR_COLOUR,
            u("Вы не успели сесть за руль, а грузовик сдвинули с площадки — он снят. Вы возвращены в конец очереди"));
    }
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

    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
    const int spot = m_haulerJobService.reservedSpotOf(playerId);
    const int vid = m_haulerJobService.vehicleIdOf(playerId);

    // Судьба грузовика: Reserved + стоит на площадке -> оставить pre-stock; иначе
    // (уведён с площадки / ведомый в любой активной фазе) -> деспавн.
    const bool busKept = (phase == HaulerJobService::Phase::Reserved) && truckOnSpot(vid, spot);

    m_haulerJobService.endShift(playerId, busKept);
    if (!busKept && vid >= 0)
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
