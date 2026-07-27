#include "Systems/HaulerJobSystem/HaulerJobSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
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

// Баланс (все именованные, в одном месте).
constexpr std::int64_t HAULER_JOB_ENTRY_FEE = 1000; // невозвратный взнос наличными при устройстве
constexpr std::int64_t PAY_PER_BOX_UNLOAD = 200;    // $ за КАЖДУЮ РАЗГРУЖЕННУЮ коробку (погрузка не платит)
constexpr std::int64_t FULL_UNLOAD_BONUS = 2000;    // $ за полную разгрузку 10/10
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
    {2218.5100f, -2235.9246f, 13.7202f}, // 1 цель посадки
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

// Маршрут ЕЗДА-ОБРАТНО: 7 замеров. Первый — выездная дорога ИЗ зоны склада (бывшая
// точка 14 плеча «туда»); финиш-точки нет — зачёт последнего (7) сразу запускает
// РАЗГРУЗКУ, его стрелка ведёт на точку выгрузки базы, парковка свободная.
const Vector3 ROUTE_BACK[HaulerJobService::BACK_LENGTH] = {
    {2740.1736f, -2403.9192f, 13.6343f}, // 1 выезд из зоны склада
    {2619.6836f, -2402.5168f, 13.6672f}, // 2
    {2528.4968f, -2321.4109f, 23.3411f}, // 3
    {2352.1296f, -2145.5469f, 18.0327f}, // 4
    {2293.9658f, -2087.4048f, 13.5040f}, // 5
    {2237.1128f, -2123.3354f, 13.5015f}, // 6
    {2204.1184f, -2156.0852f, 13.5616f}, // 7 последний -> старт разгрузки
};

// Плечо езды для фазы (Reserved/DriveOut -> туда, DriveBack -> обратно); прочие фазы
// — нет плеча (nullptr).
const Vector3 *routeForPhase(HaulerJobService::Phase phase, int &lenOut)
{
    if (phase == HaulerJobService::Phase::Reserved || phase == HaulerJobService::Phase::DriveOut)
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
                onStartWork(*player);
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

void HaulerJobSystem::onStartWork(IPlayer &player)
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
    body += "Суть работы\tВозить грузовик порт—база, грузить и разгружать коробки\n";
    body += fmt::format("Вступительный взнос\t${} наличными, невозвратный\n", HAULER_JOB_ENTRY_FEE);
    body += fmt::format("Ставка\t${} за разгруженную коробку (погрузка не оплачивается)\n", PAY_PER_BOX_UNLOAD);
    body += fmt::format("Бонус за рейс\t${} за полный рейс ({}/{})\n", FULL_UNLOAD_BONUS, HaulerJobService::BOXES_PER_LEG,
                        HaulerJobService::BOXES_PER_LEG);
    body += fmt::format("Коробки\tпо {} туда и обратно, пешком\n", HaulerJobService::BOXES_PER_LEG);
    body += "Выплата\tкопится в кошельке развозчика; на руки — через «Забрать деньги»\n";
    body += fmt::format("Посадка\t{} секунд сесть за руль и доехать до первого чекпоинта\n", RESERVE_SECONDS);
    body += fmt::format("Возврат в смене\t{} секунд вернуться за руль, если вышли по пути\n", RETURN_SECONDS);

    std::string status;
    switch (m_haulerJobService.phaseOf(playerId))
    {
    case HaulerJobService::Phase::NotWorking:
        status = "не работаете";
        break;
    case HaulerJobService::Phase::Queued:
        status = fmt::format("в очереди, место {}", m_haulerJobService.queuePositionOf(playerId));
        break;
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
    clearCounters(playerId);     // свежее окно посадки
    showDriveCheckpoint(player); // первый чекпоинт туда (Reserved -> плечо OUT, индекс 0)

    // Красный чекпоинт-маркер на закреплённый грузовик (сосуществует с race-чекпоинтом
    // маршрута — разные клиентские слоты). Снимается на посадке за руль (гейт).
    const int vid = m_haulerJobService.vehicleIdOf(playerId);
    if (IVehicle *truck = m_vehicleService.get(vid))
    {
        m_waypointService.showFor(player, *truck);
    }

    m_screenTimerService.show(player, RESERVE_SECONDS, BOARDING_TIMER_LABEL);
    player.sendClientMessage(
        INFO_COLOUR,
        u(fmt::format("За вами закреплён грузовик на площадке. У вас {} секунд сесть за руль и доехать до первого "
                      "чекпоинта",
                      RESERVE_SECONDS)));
    m_screenNoticeService.show(player, "truck reserved - get in and drive", RESERVE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
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
    HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
    if (phase != HaulerJobService::Phase::Reserved && phase != HaulerJobService::Phase::DriveOut &&
        phase != HaulerJobService::Phase::DriveBack)
    {
        return; // не в фазе езды — событие не наше
    }
    // Зачёт ТОЛЬКО за рулём СВОЕГО грузовика (серверный getDriver, не заявление клиента).
    if (!drivingOwnTruck(playerId))
    {
        return;
    }

    if (phase == HaulerJobService::Phase::Reserved)
    {
        // Посадка завершена: первый чекпоинт туда подобран за рулём.
        m_haulerJobService.completeBoarding(playerId); // Reserved -> DriveOut, площадка освобождается
        m_reserveSeconds[playerId] = 0;
        m_exitSeconds[playerId] = 0;
        m_driveIdleSeconds[playerId] = 0;
        m_screenTimerService.hide(player);
        phase = HaulerJobService::Phase::DriveOut;
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
    showBoxSource(player);
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
    showBoxSource(player);
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
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
    if (phase == HaulerJobService::Phase::Loading)
    {
        // Погрузка: берут со СЛУЧАЙНОЙ точки склада порта (общий источник координат).
        m_checkpointService.setForPlayer(player, randomWarehousePoint(), BOX_CP_RADIUS,
                                         [this](IPlayer &p)
                                         {
                                             onBoxCheckpointEnter(p);
                                         });
    }
    else if (phase == HaulerJobService::Phase::Unloading)
    {
        // Разгрузка: берут у ЗАДА грузовика (пересчёт от живой позиции/угла).
        Vector3 rear;
        if (!backOfTruck(playerId, rear))
        {
            dismiss(player, "Грузовик пропал — смена окончена", ERROR_COLOUR);
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
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
    if (phase == HaulerJobService::Phase::Loading)
    {
        // Погрузка: несут к ЗАДУ грузовика (пересчёт на каждую коробку).
        Vector3 rear;
        if (!backOfTruck(playerId, rear))
        {
            dismiss(player, "Грузовик пропал — смена окончена", ERROR_COLOUR);
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
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
    if (phase != HaulerJobService::Phase::Loading && phase != HaulerJobService::Phase::Unloading)
    {
        return; // не пешая фаза — событие не наше
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
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
    if (phase != HaulerJobService::Phase::Loading && phase != HaulerJobService::Phase::Unloading)
    {
        return; // сменил фазу/уволился во время подъёма
    }
    if (!m_haulerJobService.carryingOf(playerId))
    {
        return; // защитно
    }
    m_animationService.stop(player);                             // снять застывшую позу подъёма
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

void HaulerJobSystem::onPutdownFinished(IPlayer &player)
{
    const int playerId = player.getID();
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);
    if (phase != HaulerJobService::Phase::Loading && phase != HaulerJobService::Phase::Unloading)
    {
        return;
    }
    if (!m_haulerJobService.carryingOf(playerId))
    {
        return; // защитно (teardown/смерть сбросили несение)
    }

    detachBox(player);
    m_animationService.stop(player);
    const int count = m_haulerJobService.finishCarry(playerId); // carrying=false, ++boxCount
    m_footIdleSeconds[playerId] = 0;                            // прогресс — окно простоя с нуля

    if (phase == HaulerJobService::Phase::Loading)
    {
        if (count >= HaulerJobService::BOXES_PER_LEG)
        {
            player.sendClientMessage(INFO_COLOUR,
                                     u(fmt::format("Груз в кузове ({0}/{0}). Возвращайтесь на базу",
                                                   HaulerJobService::BOXES_PER_LEG)));
            beginDriveBackPhase(player);
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

    // Unloading: +$200 за КАЖДУЮ сданную коробку СРАЗУ в кошелёк (write-through).
    const PlayerSessionService::AccountId accountId = m_sessionService.getAccountId(playerId);
    m_haulerWalletService.add(playerId, accountId, PAY_PER_BOX_UNLOAD);
    const std::int64_t balance = m_haulerWalletService.balanceOf(playerId);
    m_screenNoticeService.show(player, fmt::format("+${}, wallet ${}", PAY_PER_BOX_UNLOAD, balance), CREDIT_POPUP_TIME,
                               CREDIT_POPUP_COLOUR);

    if (count >= HaulerJobService::BOXES_PER_LEG)
    {
        // Полная разгрузка: бонус +$2000 и цикл начинается заново с езды-туда.
        m_haulerWalletService.add(playerId, accountId, FULL_UNLOAD_BONUS);
        player.sendClientMessage(INFO_COLOUR, u(fmt::format("Разгружено {0}/{0}! Бонус +${1}",
                                                            HaulerJobService::BOXES_PER_LEG, FULL_UNLOAD_BONUS)));
        m_screenNoticeService.show(player, fmt::format("unload complete +${}", FULL_UNLOAD_BONUS), BONUS_POPUP_TIME,
                                   CREDIT_POPUP_COLOUR);
        completeCyclePhase(player);
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

void HaulerJobSystem::tickWorker(IPlayer &player)
{
    const int playerId = player.getID();
    const HaulerJobService::Phase phase = m_haulerJobService.phaseOf(playerId);

    if (phase == HaulerJobService::Phase::Reserved)
    {
        // Окно посадки: не сел за руль и не доехал до первого чекпоинта за RESERVE_SECONDS.
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

    if (phase == HaulerJobService::Phase::DriveOut || phase == HaulerJobService::Phase::DriveBack)
    {
        if (drivingOwnTruck(playerId))
        {
            m_exitSeconds[playerId] = 0;
            // Анти-AFK: за рулём, но не зачитывает чекпоинты.
            const int idle = ++m_driveIdleSeconds[playerId];
            if (idle == DRIVE_NO_PROGRESS_WARN_SECONDS)
            {
                player.sendClientMessage(
                    ERROR_COLOUR, u(fmt::format("Нет прогресса по маршруту. Через {} секунд вас уволят за простой",
                                                DRIVE_NO_PROGRESS_SECONDS - DRIVE_NO_PROGRESS_WARN_SECONDS)));
            }
            else if (idle >= DRIVE_NO_PROGRESS_SECONDS)
            {
                dismiss(player, "Вы уволены за простой на маршруте", ERROR_COLOUR);
                return;
            }
            m_screenTimerService.hide(player); // едет между чекпоинтами — активного окна нет
        }
        else
        {
            // Вышел из грузовика в фазе езды — окно возврата за руль.
            const int away = ++m_exitSeconds[playerId];
            if (away >= RETURN_SECONDS)
            {
                dismiss(player, "Вы не вернулись за руль грузовика вовремя — вы уволены", ERROR_COLOUR);
                return;
            }
            m_screenTimerService.show(player, RETURN_SECONDS - away + 1, RETURN_TIMER_LABEL);
        }
        return;
    }

    // Loading/Unloading — пеший легально; окна возврата НЕТ. Анти-AFK по сданным коробкам.
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
        player.sendClientMessage(
            ERROR_COLOUR,
            u("Вы не успели доехать до первого чекпоинта — грузовик снят. Вы возвращены в конец очереди"));
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

void HaulerJobSystem::teardownShift(IPlayer &player)
{
    const int playerId = player.getID();

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

bool HaulerJobSystem::backOfTruck(int playerId, Vector3 &out) const
{
    const int vid = m_haulerJobService.vehicleIdOf(playerId);
    IVehicle *truck = m_vehicleService.get(vid);
    if (!truck)
    {
        return false; // грузовик пропал — вызывающий увольняет
    }
    out = Geometry::backOf(truck->getPosition(), truck->getZAngle(), TRUCK_REAR_OFFSET);
    return true;
}

Vector3 HaulerJobSystem::randomWarehousePoint()
{
    // Единый источник координат склада (общий с портом) — PortJobService::dropPositions.
    const std::array<Vector3, PortJobService::DROP_COUNT> &pts = PortJobService::dropPositions();
    std::uniform_int_distribution<int> dist(0, PortJobService::DROP_COUNT - 1);
    return pts[dist(m_rng)];
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
