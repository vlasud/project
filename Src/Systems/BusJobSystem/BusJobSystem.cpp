#include "Systems/BusJobSystem/BusJobSystem.h"

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
// Игровые цвета сообщений (как в PortJobSystem/ParkingSystem).
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Пикап трудоустройства (info-икона «i», сервис-точка, подбор по касанию — та же
// визуальная конвенция, что пикап порта/парковки). Координаты — от геймдизайна.
const Vector3 EMPLOY_PICKUP_POS{1237.2563f, -1813.6050f, 13.4313f};
constexpr int PICKUP_MODEL = 1239;
constexpr PickupType PICKUP_TYPE = 1;
constexpr int JOB_MAP_ICON = 46; // автобус на миникарте

// Автобус: модель 431. Три площадки депо выровнены в ряд, угол 180 (от геймдизайна).
constexpr int BUS_MODEL = 431;
constexpr float SLOT_ANGLE = 180.0f;
const Vector3 SLOT_POS[BusJobService::SLOT_COUNT] = {
    {1262.58f, -1800.01f, 13.5162f},
    {1269.67f, -1800.01f, 13.5049f},
    {1276.76f, -1800.01f, 13.4933f},
};

// Порог «этот автобус ещё стоит на своей площадке» (busOnSpot): горизонтальная (XY)
// дистанция ОРИГИНА конкретного автобуса до точки. Origin стоящего автобуса совпадает с
// точкой; уехал — origin ушёл за порог. Занятость точки ЛЮБЫМ транспортом (для
// пере-стока) считается отдельно, многопробным критерием ниже.
constexpr float SPOT_RADIUS = 7.0f;

// Критерий «площадка физически чиста для пере-стока» — многопробный, как
// ParkingSystem::isSpotFree: центр + пробы вдоль ориентации места (SLOT_ANGLE), радиус
// на каждую пробу. Автобус длинный (~13 м) и лежит телом вдоль оси места; одна
// центральная проба поймала бы лишь origin, пропустив длинное ТС, стоящее телом на
// площадке origin'ом за радиусом. Пробы идут ⊥ ряду площадок (тело вдоль Y, соседи по X
// ~7.09 м) -> кросс-детекта соседних автобусов нет без хрупкой маржи.
constexpr float SPOT_OCCUPIED_RADIUS = 3.0f;
constexpr float SPOT_PROBE_OFFSET = 4.0f;

// Радиусы race-чекпоинтов маршрута. Автобус большой — move чуть шире стандартного
// 3.0, stop ещё шире (нужно въехать и встать в зоне на 10 с). open.mp шлёт size=radius
// одинаково для обычного и race-чекпоинта (checkpoint.hpp), пересчёт диаметр/радиус не
// нужен — прежний ВИДИМЫЙ размер сохраняется.
constexpr float MOVE_CP_RADIUS = 4.0f;
constexpr float STOP_CP_RADIUS = 6.0f;

// Баланс (все именованные, в одном месте).
constexpr std::int64_t PAY_PER_CHECKPOINT = 50; // $ за зачтённый чекпоинт (move и stop)
constexpr std::int64_t LAP_BONUS = 2000;        // $ за полный круг (54 подряд)
// Невозвратный вступительный взнос наличными при устройстве (денежный сток): барьер
// входа в премиальную работу. Берётся ОДИН раз при новом устройстве (не при
// повторной постановке в очередь после провала посадки), назад не возвращается.
constexpr std::int64_t BUS_JOB_ENTRY_FEE = 1000;
constexpr int RESERVE_SECONDS = 30;             // окно посадки от резерва
constexpr int RETURN_SECONDS = 30;              // окно возврата за руль после выхода
constexpr int STOP_DWELL_SECONDS = 10;          // простой в зоне stop-чекпоинта
// Анти-AFK за рулём: за рулём своего автобуса, но без единого зачтённого чекпоинта за
// NO_PROGRESS_SECONDS -> увольнение (порог заведомо выше любого легального
// межчекпоинтного отрезка), с предупреждением за 30 с.
constexpr int NO_PROGRESS_SECONDS = 240;
constexpr int NO_PROGRESS_WARN_SECONDS = 210;
constexpr Milliseconds DEPOT_TICK{1000}; // период общего таймера депо

// Экранные попапы — ТОЛЬКО английский (как все попапы проекта).
constexpr Milliseconds RESERVE_POPUP_TIME{4000};
constexpr Milliseconds CREDIT_POPUP_TIME{2500};
constexpr Milliseconds LAP_POPUP_TIME{3500};
constexpr Milliseconds FAIL_POPUP_TIME{3000};
const Colour NEUTRAL_POPUP_COLOUR{0xFF, 0xFF, 0xFF, 0xFF};
const Colour CREDIT_POPUP_COLOUR{0x90, 0xEE, 0x90, 0xFF};
const Colour ERROR_POPUP_COLOUR{0xFF, 0x5A, 0x5A, 0xFF};

// Метки GUI-таймера обратного отсчёта (ScreenTimerService, верх-центр) — ТОЛЬКО
// English (как все экранные надписи). Остаток числовой (M:SS), метка поясняет окно.
// Звук прогресса: играет вместе с попапом «что-то засчитано» (зачёт чекпоинта,
// круг). На отказы и провалы НЕ вешаем — там свой красный попап.
constexpr std::uint32_t PROGRESS_SOUND = 17803;

const char *const BOARDING_TIMER_LABEL = "BOARDING";
const char *const RETURN_TIMER_LABEL = "RETURN";
const char *const STOP_TIMER_LABEL = "BUS STOP";

// Маршрут: 54 race-чекпоинта по кругу. stop-остановки — #3/#14/#28/#47
// (1-based); после последнего — снова первый. Координаты — от геймдизайна.
struct RouteNode
{
    Vector3 pos;
    bool stop;
};
const RouteNode ROUTE[BusJobService::ROUTE_LENGTH] = {
    {{1271.9669f, -1849.4620f, 13.4928f}, false}, // 1
    {{1204.3531f, -1849.8187f, 13.4867f}, false}, // 2
    {{1183.2498f, -1748.3488f, 13.5001f}, true},  // 3  stop
    {{1186.3951f, -1716.0330f, 13.5494f}, false}, // 4
    {{1282.1388f, -1714.5015f, 13.4832f}, false}, // 5
    {{1295.1053f, -1781.0251f, 13.4829f}, false}, // 6
    {{1321.6678f, -1854.8718f, 13.4831f}, false}, // 7
    {{1374.3870f, -1874.1886f, 13.4838f}, false}, // 8
    {{1491.2411f, -1874.7007f, 13.4833f}, false}, // 9
    {{1673.6343f, -1870.7778f, 13.4909f}, false}, // 10
    {{1691.7064f, -1825.4160f, 13.4803f}, false}, // 11
    {{1726.0637f, -1816.9590f, 13.4617f}, false}, // 12
    {{1814.9225f, -1835.1821f, 13.4970f}, false}, // 13
    {{1818.1403f, -1909.3845f, 13.4945f}, true},  // 14 stop
    {{1819.3329f, -1930.2184f, 13.4791f}, false}, // 15
    {{1935.4126f, -1934.7410f, 13.4841f}, false}, // 16
    {{1959.3098f, -1965.0028f, 13.7071f}, false}, // 17
    {{1958.9502f, -2095.0874f, 13.4946f}, false}, // 18
    {{2024.4608f, -2112.6982f, 13.4609f}, false}, // 19
    {{2130.1111f, -2117.7505f, 13.4299f}, false}, // 20
    {{2185.9353f, -2156.0598f, 13.4844f}, false}, // 21
    {{2232.9675f, -2134.4207f, 13.4427f}, false}, // 22
    {{2273.1821f, -2094.4277f, 13.6335f}, false}, // 23
    {{2343.3286f, -2143.6062f, 16.3223f}, false}, // 24
    {{2517.8804f, -2318.1868f, 23.8149f}, false}, // 25
    {{2610.6318f, -2406.2485f, 13.6038f}, false}, // 26
    {{2669.2588f, -2407.2461f, 13.5595f}, false}, // 27
    {{2680.6409f, -2452.6550f, 13.6065f}, true},  // 28 stop
    {{2681.0957f, -2489.8799f, 13.6149f}, false}, // 29
    {{2593.5845f, -2501.5969f, 13.5923f}, false}, // 30
    {{2495.3467f, -2503.9365f, 13.5719f}, false}, // 31
    {{2482.1682f, -2592.7637f, 13.5868f}, false}, // 32
    {{2474.0391f, -2657.2786f, 13.5863f}, false}, // 33
    {{2329.2339f, -2660.1460f, 13.6026f}, false}, // 34
    {{2231.9719f, -2654.2617f, 13.4945f}, false}, // 35
    {{2227.4048f, -2514.3418f, 13.4758f}, false}, // 36
    {{2198.1655f, -2492.6362f, 13.4993f}, false}, // 37
    {{2177.4922f, -2462.7192f, 13.4739f}, false}, // 38
    {{2205.5730f, -2373.7170f, 13.4766f}, false}, // 39
    {{2270.6633f, -2308.1177f, 13.4754f}, false}, // 40
    {{2282.8328f, -2247.0420f, 13.6563f}, false}, // 41
    {{2232.9092f, -2195.0896f, 13.4086f}, true},  // 42 stop
    {{2138.1199f, -2115.0918f, 13.4852f}, false}, // 43
    {{1970.0699f, -2107.7026f, 13.4805f}, false}, // 44
    {{1964.2185f, -1949.8091f, 13.7426f}, false}, // 45
    {{1841.4648f, -1929.8314f, 13.4848f}, false}, // 46
    {{1825.4285f, -1846.6141f, 13.5138f}, true},  // 47 stop
    {{1824.3652f, -1754.6309f, 13.4832f}, false}, // 48
    {{1768.3521f, -1729.5741f, 13.4832f}, false}, // 49
    {{1561.2189f, -1729.8073f, 13.4833f}, false}, // 50
    {{1412.4025f, -1729.2506f, 13.4938f}, false}, // 51
    {{1386.1445f, -1793.0925f, 13.4821f}, false}, // 52
    {{1386.7429f, -1859.4707f, 13.4827f}, false}, // 53
    {{1329.1086f, -1850.6317f, 13.4861f}, false}, // 54
};

float distanceSq(const Vector3 &a, const Vector3 &b)
{
    const Vector3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

// Горизонтальная (XY) дистанция² — для «автобус на площадке» (Z игнорируем, автобус
// оседает на грунт со своей Z), как anyVehicleNear.
float distanceSq2D(const Vector3 &a, const Vector3 &b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}
} // namespace

BusJobSystem::BusJobSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_busJobService(serviceRegister.getService<BusJobService>()),
      m_busWalletService(serviceRegister.getService<BusWalletService>()),
      m_jobWalletService(serviceRegister.getService<JobWalletService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_checkpointService(serviceRegister.getService<CheckpointService>()),
      m_pickupService(serviceRegister.getService<PickupService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_healthService(serviceRegister.getService<PlayerHealthService>()),
      m_timers(serviceRegister.getService<TimerService>()),
      m_screenNoticeService(serviceRegister.getService<ScreenNoticeService>()),
      m_screenTimerService(serviceRegister.getService<ScreenTimerService>()),
      m_mapIconService(serviceRegister.getService<MapIconService>()),
      m_audioService(serviceRegister.getService<AudioService>()),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>()),
      m_navLockService(serviceRegister.getService<NavigationLockService>())
{
    // Загрузка персистентного кошелька автобусника по старту сессии (serial-guard).
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            loadWallet(player, session);
        });

    // Конец сессии (в т.ч. дисконнект) в ЛЮБОЙ фазе: тихий teardown смены (деспавн
    // едущего автобуса, снятие резерва, выход из очереди) + сброс ОЗУ-кэша кошелька
    // (баланс в БД остаётся). Ре-используемый playerId не наследует чужую смену.
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            onSessionEnd(player);
        });

    // Смерть в смене серверно-авторитетна (не клиентский onPlayerDeath) — немедленное
    // увольнение.
    m_healthService.subscribeDeath(
        [this](IPlayer &player)
        {
            onPlayerDeath(player);
        });

    // Гейт водителя (единственный слой привязки): за руль резервного/едущего автобуса
    // пускается ТОЛЬКО его работник, в свободный стоящий pre-stock — никто (перенос
    // резерва убран). Пассажирские места не гейтим — свободны для всех. При отказе
    // VehicleService сам высаживает игрока (removeFromVehicle) — серверный бэкстоп.
    m_vehicleService.subscribeDriverGate([this](IPlayer &player, IVehicle &vehicle)
                                         { return onDriverGate(player, vehicle); });

    // Универсальный выход с работы (/stopjob): реестр знает, чем игрок занят, а
    // увольняет наш же обработчик пикапа — второй логики увольнения не появляется.
    serviceRegister.getService<JobDismissService>().registerJob(
        "водитель автобуса",
        fmt::format("Автобус будет снят, а взнос ${} не возвращается — устройство заново снова платное. "
                    "Заработок в кошельке автобусника сохранится, его можно забрать у пикапа работы.",
                    BUS_JOB_ENTRY_FEE),
        [this](int playerId)
        {
            return m_busJobService.isWorking(playerId);
        },
        [this](IPlayer &player)
        {
            onFinishWork(player);
        });

    // Кошелёк этой работы — в справочный список (/jobwallet). Только баланс: выдача
    // остаётся на пикапе работы, туда за деньгами и едут.
    serviceRegister.getService<JobWalletService>().registerWallet(
        "водитель автобуса", "пикап работы в депо автобусов",
        [this](int playerId)
        {
            return m_busWalletService.balanceOf(playerId);
        });
}

void BusJobSystem::initialize(IComponentList * /*components*/)
{
    // PickupSystem/MapIconSystem/VehicleSystem уже получили компоненты (порядок реестра
    // — BusJobSystem после них).
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

    // Автобусов в депо на старте НЕТ: площадки пустуют, машина появляется на
    // площадке только под конкретного работника (см. onReserved).

    // Один общий per-second таймер депо на весь сервер: НИКОГДА не отменяется (в т.ч.
    // из своего колбэка). Депо-часть O(SLOT_COUNT), окна работников — O(активных).
    m_depotTimer = m_timers.setInterval(DEPOT_TICK,
                                        [this]()
                                        {
                                            onDepotTick();
                                        });
}

// ------------------------------------------------------------------ пикап + диалог

void BusJobSystem::onPickup(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }

    // Все пункты видны ВСЕГДА (правило проекта): гейт — в обработчике по клику
    // сообщением, не скрытием. Порядок как у порта: рабочие пункты, денежный
    // рядом с ними, справка последней.
    const std::string body = "Начать работу\nЗавершить работу\nЗабрать деньги\nИнформация";
    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Работа — водитель автобуса", body, "Выбрать", "Закрыть"),
        [this, playerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return; // игрок вышел / закрыл
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

void BusJobSystem::onStartWork(IPlayer &player)
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
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из транспорта, чтобы устроиться водителем автобуса"));
        return;
    }
    // Уже в смене/очереди — повторного устройства нет, взнос НЕ берём (гейт до
    // списания). Провал посадки возвращает в очередь работником — тоже мимо этой ветки.
    if (m_busJobService.isWorking(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы уже работаете водителем автобуса"));
        return;
    }
    // Взаимное исключение работ: единственный чекпоинт-слот и лок навигации принадлежат
    // ТЕКУЩЕЙ смене. isWorking(автобус) выше уже ложно, значит держатель лока — ДРУГАЯ
    // работа (порт): устройство поверх неё затёрло бы её лок/маркер и оставило первую
    // смену без чекпоинта. Гейтим ДО списания взноса.
    if (m_navLockService.isLocked(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Нельзя устроиться водителем: {}", m_navLockService.lockReason(playerId))));
        return;
    }

    // Невозвратный вступительный взнос НАЛИЧНЫМИ (PlayerMoneyService, не кошелёк работы):
    // проверка наличия + списание АТОМАРНО в момент клика и ДО перевода в работники
    // (иначе при нехватке денег повисла бы фаза). Взнос — сток, назад не возвращается.
    const unsigned long long cash = m_moneyService.getMoney(playerId);
    if (!m_moneyService.take(player, static_cast<unsigned long long>(BUS_JOB_ENTRY_FEE)))
    {
        player.sendClientMessage(
            ERROR_COLOUR,
            u(fmt::format("Недостаточно наличных: вступительный взнос ${} (у вас ${})", BUS_JOB_ENTRY_FEE, cash)));
        return;
    }
    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Вступительный взнос ${} списан (не возвращается)", BUS_JOB_ENTRY_FEE)));

    // isWorking выше уже отсёк AlreadyWorking -> startWork возвращает только Reserved/Queued.
    const BusJobService::StartOutcome outcome = m_busJobService.startWork(
        playerId,
        [this](int spot)
        {
            return spotClear(spot);
        });
    // Лок навигации на ВСЮ смену (устройство удалось — и Reserved, и Queued это уже
    // смена; лок держится и в очереди). Освобождается на любом её конце (teardownShift/
    // выход из очереди). Взятие гасит активный GPS-маркер игрока.
    m_navLockService.acquire(playerId, "идёт смена водителя автобуса");
    if (outcome.result == BusJobService::StartResult::Queued)
    {
        player.sendClientMessage(
            INFO_COLOUR,
            u(fmt::format("Свободных автобусов в депо сейчас нет. Вы в очереди, место {}", outcome.queuePosition)));
        return;
    }
    // Reserved — свободный стоящий автобус закреплён за игроком.
    onReserved(player);
}

void BusJobSystem::onFinishWork(IPlayer &player)
{
    const int playerId = player.getID();
    const BusJobService::Phase phase = m_busJobService.phaseOf(playerId);

    if (phase == BusJobService::Phase::NotWorking)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы сейчас не работаете водителем автобуса"));
        return;
    }
    if (phase == BusJobService::Phase::Queued)
    {
        m_busJobService.endShift(playerId); // в очереди автобуса нет — снимать нечего
        clearCounters(playerId);
        m_navLockService.release(playerId); // выход из очереди = конец смены (teardownShift здесь не проходит)
        player.sendClientMessage(INFO_COLOUR, u("Вы вышли из очереди на автобус"));
        notifyQueueShift(); // стоявшие позади подвинулись
        return;
    }
    // Reserved/Driving — штатное увольнение (кошелёк остаётся).
    dismiss(player, "Смена окончена. Заработок сохранён в кошельке автобусника — заберите через «Забрать деньги»",
            INFO_COLOUR);
}

void BusJobSystem::onWithdrawMoney(IPlayer &player)
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

    if (m_busWalletService.balanceOf(playerId) <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Забирать нечего"));
        return;
    }

    // withdraw обнуляет кэш СРАЗУ — повторный клик/второй колбэк не выдаст дважды.
    const std::int64_t amount = m_busWalletService.withdraw(playerId, session->accountId);
    if (amount <= 0)
    {
        return; // гонка кликов — уже забрано
    }

    m_moneyService.giveMoney(player, static_cast<unsigned long long>(amount));
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Вы забрали ${} из кошелька автобусника", amount)));
}

void BusJobSystem::showInfo(IPlayer &player)
{
    const int playerId = player.getID();

    std::string body = "Поле\tЗначение\n";
    body += "Суть работы\tВозить автобус по кольцевому маршруту (54 чекпоинта)\n";
    body += fmt::format("Вступительный взнос\t${} наличными, невозвратный\n", BUS_JOB_ENTRY_FEE);
    body += fmt::format("Ставка\t${} за чекпоинт\n", PAY_PER_CHECKPOINT);
    body += fmt::format("Бонус за круг\t${}\n", LAP_BONUS);
    body += "Выплата\tкопится в кошельке автобусника; на руки — через «Забрать деньги»\n";
    body += fmt::format("Посадка\t{} секунд сесть за руль и доехать до первого чекпоинта\n", RESERVE_SECONDS);
    body += fmt::format("Возврат в смене\t{} секунд вернуться за руль, если вышли\n", RETURN_SECONDS);
    body += fmt::format("Остановка\tпростоять {} секунд за рулём у чекпоинта-остановки\n", STOP_DWELL_SECONDS);

    std::string status;
    switch (m_busJobService.phaseOf(playerId))
    {
    case BusJobService::Phase::NotWorking:
        status = "не работаете";
        break;
    case BusJobService::Phase::Queued:
        status = fmt::format("в очереди, место {}", m_busJobService.queuePositionOf(playerId));
        break;
    case BusJobService::Phase::Reserved:
        status = "автобус закреплён, идёт посадка";
        break;
    case BusJobService::Phase::Driving:
        status = fmt::format("за рулём, чекпоинт {}/{}", m_busJobService.checkpointIndexOf(playerId) + 1,
                             BusJobService::ROUTE_LENGTH);
        break;
    }
    body += fmt::format("Ваш статус\t{}\n", status);
    body += fmt::format("В кошельке автобусника\t${}", m_busWalletService.balanceOf(playerId));

    m_dialogService.show(player,
                         makeDialog(DialogStyle_TABLIST_HEADERS, "Работа водителем автобуса", body, "Назад", ""),
                         [](DialogResponse, int, StringView)
                         {
                         });
}

// ------------------------------------------------------------------ гейт водителя

bool BusJobSystem::onDriverGate(IPlayer &player, IVehicle &vehicle)
{
    const int vid = vehicle.getID();
    // Дешёвый фильтр горячего пути: наши автобусы — всегда Owner::Work. Прочий
    // транспорт (личный/фракционный/парковка) — не наш гейт, без скана игроков/площадок.
    if (m_vehicleService.getOwner(vid) != VehicleService::Owner::Work)
    {
        return true;
    }
    const int playerId = player.getID();

    const int worker = m_busJobService.workerOfVehicle(vid);
    if (worker >= 0)
    {
        if (worker == playerId)
        {
            // Сел за руль своего автобуса — маркер-указатель на него больше не нужен.
            m_waypointService.clearFor(player);
            // Посадка за руль открывает маршрут. Гейт зовётся уже ПОСЛЕ привязки
            // водителя (getDriver наш), поэтому переход опирается на серверный факт.
            if (m_busJobService.phaseOf(playerId) == BusJobService::Phase::Reserved)
            {
                completeBoarding(player);
            }
            return true; // свой закреплённый/угнанный автобус
        }
        player.sendClientMessage(ERROR_COLOUR,
                                 u("Это служебный автобус — за руль пускают только назначенного водителя"));
        return false;
    }
    return true; // не наш автобус — гейт не наш
}

// ------------------------------------------------------------------ резерв/очередь

void BusJobSystem::onReserved(IPlayer &player)
{
    const int playerId = player.getID();
    clearCounters(playerId); // свежее окно посадки (reserveSeconds с нуля)

    // Автобус подаётся ПОД РАБОТНИКА: депо стоит пустым, машина появляется на его
    // площадке только сейчас. Не смогли подать (пул машин полон) — площадку не держим,
    // возвращаем в очередь: занятая площадка без машины заблокировала бы депо.
    if (!spawnForWorker(player))
    {
        m_busJobService.requeueTail(playerId);
        player.sendClientMessage(ERROR_COLOUR, u("Автобус сейчас не подать. Вы возвращены в очередь"));
        return;
    }

    // Маршрут в фазе подачи НЕ показываем: цель ровно одна — свой автобус. Первый
    // чекпоинт появится, когда игрок сядет за руль (completeBoarding).

    // Красный чекпоинт-маркер на закреплённый автобус (найти свой среди стоящих) через
    // общий VehicleWaypointService (обычный слот) — сосуществует с race-чекпоинтом
    // маршрута (разные клиентские слоты). Единый слот на игрока: если висел указатель
    // парковки, он перетирается (последний выигрывает). Снимается на посадке за руль
    // (гейт), провале, увольнении, дисконнекте.
    const int vid = m_busJobService.vehicleIdOf(playerId);
    if (IVehicle *bus = m_vehicleService.get(vid))
    {
        m_waypointService.showFor(player, *bus);
    }

    // GUI-таймер выезда: полный остаток RESERVE_SECONDS, дальше тик обновляет. Окно
    // закрывает ОТЪЕЗД с площадки, а не посадка за руль — площадка нужна следующему.
    m_screenTimerService.show(player, RESERVE_SECONDS, BOARDING_TIMER_LABEL);

    player.sendClientMessage(
        INFO_COLOUR,
        u(fmt::format("За вами закреплён автобус на площадке. У вас {} секунд сесть за руль и доехать до первого "
                      "чекпоинта",
                      RESERVE_SECONDS)));
    m_screenNoticeService.show(player, "bus reserved - get in and drive", RESERVE_POPUP_TIME, NEUTRAL_POPUP_COLOUR);
}

void BusJobSystem::completeBoarding(IPlayer &player)
{
    const int playerId = player.getID();
    m_busJobService.completeBoarding(playerId); // Reserved -> Driving (площадку НЕ отпускает)
    m_exitSeconds[playerId] = 0;
    m_waypointService.clearFor(player);
    showRouteCheckpoint(player); // теперь маршрут: первый чекпоинт
    player.sendClientMessage(INFO_COLOUR,
                             u("Вы за рулём — следуйте по чекпоинтам. Отъезжайте с площадки, она нужна другим"));
}

void BusJobSystem::pumpQueue()
{
    // Продвигать голову очереди на все свободные стоящие автобусы (пере-сток/снятый
    // резерв). Ограничено min(длины очереди, SLOT_COUNT) — не больше SLOT_COUNT итераций.
    bool promoted = false;
    for (;;)
    {
        const BusJobService::Promotion promotion = m_busJobService.promoteQueue(
            [this](int spot)
            {
                return spotClear(spot);
            });
        if (promotion.playerId < 0)
        {
            break; // очередь пуста / свободных стоящих автобусов нет
        }
        IPlayer *next = m_core.getPlayers().get(promotion.playerId);
        if (!next)
        {
            // Продвинутый работник пропал (очередь чистится на дисконнекте — почти
            // недостижимо): снять площадку и продолжить (автобус ему ещё не подавали).
            m_busJobService.endShift(promotion.playerId);
            clearCounters(promotion.playerId);
            m_navLockService.release(promotion.playerId); // на всякий — лок ушедшего снять
            continue;
        }
        onReserved(*next);
        promoted = true;
    }
    if (promoted)
    {
        notifyQueueShift(); // остальным ждущим — обновлённое место в очереди
    }
}

void BusJobSystem::notifyQueueShift()
{
    // Очередь сдвинулась (голова получила автобус / кто-то вышел) — уведомить
    // остальных ждущих об их новом месте; голову очереди — что она следующая.
    const std::vector<int> queued = m_busJobService.queuedPlayers();
    for (int i = 0; i < static_cast<int>(queued.size()); ++i)
    {
        IPlayer *waiting = m_core.getPlayers().get(queued[i]);
        if (!waiting)
        {
            continue;
        }
        if (i == 0)
        {
            waiting->sendClientMessage(INFO_COLOUR, u("Очередь на автобус продвинулась — вы следующий"));
        }
        else
        {
            waiting->sendClientMessage(INFO_COLOUR,
                                       u(fmt::format("Очередь на автобус продвинулась, ваше место {}", i + 1)));
        }
    }
}

// ------------------------------------------------------------------ маршрут

void BusJobSystem::showRouteCheckpoint(IPlayer &player)
{
    const int index = m_busJobService.checkpointIndexOf(player.getID());
    if (index < 0 || index >= BusJobService::ROUTE_LENGTH)
    {
        return; // защитно
    }
    const RouteNode &node = ROUTE[index];
    if (node.stop)
    {
        // Остановка: RACE_FINISH — финиш-маркер без стрелки («здесь стоим»). nextPosition
        // ОБЯЗАН быть пустым (0,0,0), иначе клиент покажет RACE_NORMAL со стрелкой.
        m_checkpointService.setRaceForPlayer(player, RaceCheckpointType::RACE_FINISH, node.pos,
                                             Vector3{0.0f, 0.0f, 0.0f}, STOP_CP_RADIUS,
                                             [this](IPlayer &p)
                                             {
                                                 onCheckpointEnter(p);
                                             });
        return;
    }
    // move: RACE_NORMAL со стрелкой на СЛЕДУЮЩУЮ точку маршрута по кругу (после
    // последней — снова первая) — водитель всегда видит, куда ехать дальше.
    const int nextIndex = (index + 1) % BusJobService::ROUTE_LENGTH;
    m_checkpointService.setRaceForPlayer(player, RaceCheckpointType::RACE_NORMAL, node.pos, ROUTE[nextIndex].pos,
                                         MOVE_CP_RADIUS,
                                         [this](IPlayer &p)
                                         {
                                             onCheckpointEnter(p);
                                         });
}

void BusJobSystem::onCheckpointEnter(IPlayer &player)
{
    const int playerId = player.getID();
    const BusJobService::Phase phase = m_busJobService.phaseOf(playerId);
    if (phase != BusJobService::Phase::Reserved && phase != BusJobService::Phase::Driving)
    {
        return; // не в активной смене — событие не наше
    }
    // Зачёт ТОЛЬКО за рулём СВОЕГО автобуса (пешком/на другом ТС не считается —
    // серверный getDriver, не клиентское заявление).
    if (!drivingOwnBus(playerId))
    {
        return;
    }

    if (phase == BusJobService::Phase::Reserved)
    {
        // Страховка: штатно фазу закрывает гейт руля в момент посадки, но за руль
        // можно попасть и мимо него (серверная посадка putInVehicle).
        completeBoarding(player);
    }

    const int index = m_busJobService.checkpointIndexOf(playerId);
    if (index < 0 || index >= BusJobService::ROUTE_LENGTH)
    {
        return;
    }
    if (ROUTE[index].stop)
    {
        // Остановка: зачёт по простою 10 с в зоне (ведёт per-second тик по принятой
        // позиции). Вход лишь показывает подсказку.
        m_dwellSeconds[playerId] = 0;
        player.sendClientMessage(
            INFO_COLOUR,
            u(fmt::format("Остановка автобуса: постойте {} секунд, не покидая руль", STOP_DWELL_SECONDS)));
        // GUI-таймер остановки: полный остаток STOP_DWELL_SECONDS, дальше тик обновляет
        // (dwell по принятой позиции); выезд из зоны сбросит и погасит бар.
        m_screenTimerService.show(player, STOP_DWELL_SECONDS, STOP_TIMER_LABEL);
        return;
    }
    // move — мгновенный зачёт.
    creditAndAdvance(player);
}

void BusJobSystem::creditAndAdvance(IPlayer &player)
{
    const int playerId = player.getID();
    const PlayerSessionService::AccountId accountId = m_sessionService.getAccountId(playerId);

    // +$50 за зачтённый чекпоинт СРАЗУ в персистентный кошелёк (write-through).
    m_busWalletService.add(playerId, accountId, PAY_PER_CHECKPOINT);

    const int newIndex = m_busJobService.advanceCheckpoint(playerId);
    m_dwellSeconds[playerId] = 0; // новый чекпоинт — простой с нуля
    m_idleSeconds[playerId] = 0;  // есть прогресс по маршруту — окно простоя с нуля

    if (newIndex == 0)
    {
        // Круг замкнулся: бонус +$2000 (топливо у автобусов бесконечное — не заправляем).
        m_busWalletService.add(playerId, accountId, LAP_BONUS);
        player.sendClientMessage(INFO_COLOUR, u(fmt::format("Круг завершён! Бонус +${}", LAP_BONUS)));
        m_screenNoticeService.show(player, fmt::format("lap complete +${}", LAP_BONUS), LAP_POPUP_TIME,
                                   CREDIT_POPUP_COLOUR);
    }
    else
    {
        const std::int64_t balance = m_busWalletService.balanceOf(playerId);
        m_screenNoticeService.show(player, fmt::format("+${}, wallet ${}", PAY_PER_CHECKPOINT, balance),
                                   CREDIT_POPUP_TIME, CREDIT_POPUP_COLOUR);
    }
    m_audioService.playSound(player, PROGRESS_SOUND); // попап прогресса всегда со звуком

    m_screenTimerService.hide(player); // зачёт — активного окна отсчёта больше нет
    showRouteCheckpoint(player);       // следующая цель
}

// ------------------------------------------------------------------ таймер депо

void BusJobSystem::onDepotTick()
{
    // 1) Депо: освободить площадки, с которых автобус уехал. O(SLOT_COUNT).
    releaseDepartedSpots();
    // 2) Продвижение очереди на освободившиеся площадки.
    pumpQueue();
    // 3) Окна активных работников (посадка/возврат/остановка/анти-AFK).
    tickActiveWorkers();
}

void BusJobSystem::releaseDepartedSpots()
{
    for (int i = 0; i < BusJobService::SLOT_COUNT; ++i)
    {
        const int holder = m_busJobService.holderOfSpot(i);
        if (holder < 0)
        {
            continue; // площадка и так свободна
        }
        // Площадку держит машина работника, пока физически стоит на ней. Уехал (или
        // машины не стало) — площадка идёт следующему из очереди. Сам работник при
        // этом остаётся со своим автобусом: это НЕ конец смены.
        const int vid = m_busJobService.vehicleIdOf(holder);
        if (vid < 0 || !busOnSpot(vid, i))
        {
            m_busJobService.releaseSpot(holder);
            // Выехал — окно выезда закрыто.
            m_reserveSeconds[holder] = 0;
            if (IPlayer *worker = m_core.getPlayers().get(holder))
            {
                m_screenTimerService.hide(*worker);
            }
        }
    }
}

bool BusJobSystem::spawnForWorker(IPlayer &player)
{
    const int playerId = player.getID();
    const int spot = m_busJobService.reservedSpotOf(playerId);
    if (spot < 0 || spot >= BusJobService::SLOT_COUNT)
    {
        return false;
    }
    IVehicle *bus = m_vehicleService.create(BUS_MODEL, SLOT_POS[spot], SLOT_ANGLE, -1, -1,
                                            VehicleService::Owner::Work, -1);
    if (!bus)
    {
        LogManager::log(Error, "BusJobSystem: vehicle pool full, bus not spawned");
        return false;
    }
    // Рабочий транспорт: бесконечное топливо (бак не расходуется, автобус не глохнет
    // от пустого бака). Единственный путь спавна автобусов — здесь.
    m_vehicleService.setInfiniteFuel(*bus, true);
    m_busJobService.setStanding(spot, bus->getID());
    m_busJobService.setVehicle(playerId, bus->getID());
    return true;
}

void BusJobSystem::tickActiveWorkers()
{
    // Активных работников (Reserved/Driving) может быть сколько угодно — обходим онлайн
    // и тикаем только их. Дисконнект/увольнение внутри тика не трогают пул игроков;
    // вновь продвинутый в этом же тике работник максимум получит +1 с окна (±1 с
    // несущественно).
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
        const BusJobService::Phase phase = m_busJobService.phaseOf(playerId);
        if (phase == BusJobService::Phase::Reserved || phase == BusJobService::Phase::Driving)
        {
            tickWorker(*player);
        }
    }
}

void BusJobSystem::tickWorker(IPlayer &player)
{
    const int playerId = player.getID();
    const BusJobService::Phase phase = m_busJobService.phaseOf(playerId);

    // Окно выезда идёт, пока за игроком числится ПЛОЩАДКА, а не пока длится фаза
    // Reserved: сесть за руль он мог уже секунду назад (маршрут открыт), но площадка
    // занята его автобусом, и следующему из очереди её не отдать.
    if (m_busJobService.reservedSpotOf(playerId) >= 0)
    {
        const int elapsed = ++m_reserveSeconds[playerId];
        if (elapsed >= RESERVE_SECONDS)
        {
            failBoarding(player); // гасит GUI-таймер внутри
            return;
        }
        m_screenTimerService.show(player, RESERVE_SECONDS - elapsed, BOARDING_TIMER_LABEL);
        return; // маршрутные окна не ведём, пока не выехал
    }
    if (phase != BusJobService::Phase::Driving)
    {
        return; // Queued/NotWorking сюда не приходят — защитно
    }

    if (drivingOwnBus(playerId))
    {
        m_exitSeconds[playerId] = 0;
        // Анти-AFK: сидит за рулём, но не зачитывает чекпоинты. Предупреждаем за 30 с,
        // затем увольняем (деспавн автобуса — не копим брошенные машины на маршруте).
        const int idle = ++m_idleSeconds[playerId];
        if (idle == NO_PROGRESS_WARN_SECONDS)
        {
            player.sendClientMessage(
                ERROR_COLOUR, u(fmt::format("Нет прогресса по маршруту. Через {} секунд вас уволят за простой",
                                            NO_PROGRESS_SECONDS - NO_PROGRESS_WARN_SECONDS)));
        }
        else if (idle >= NO_PROGRESS_SECONDS)
        {
            dismiss(player, "Вы уволены за простой на маршруте", ERROR_COLOUR);
            return; // состояние смены снято — со stale-индексом ниже работать нельзя
        }
        const int index = m_busJobService.checkpointIndexOf(playerId);
        if (index >= 0 && index < BusJobService::ROUTE_LENGTH && ROUTE[index].stop)
        {
            // Простой зачитывается ТОЛЬКО в зоне stop-чекпоинта по ПРИНЯТОЙ сервером
            // позиции (клиенту не верим); уехал из зоны — сброс.
            const Vector3 pos = m_locationService.getPosition(playerId);
            if (distanceSq(pos, ROUTE[index].pos) <= STOP_CP_RADIUS * STOP_CP_RADIUS)
            {
                const int dwell = ++m_dwellSeconds[playerId];
                if (dwell >= STOP_DWELL_SECONDS)
                {
                    m_dwellSeconds[playerId] = 0;
                    creditAndAdvance(player); // гасит GUI-таймер внутри
                }
                else
                {
                    m_screenTimerService.show(player, STOP_DWELL_SECONDS - dwell, STOP_TIMER_LABEL);
                }
            }
            else
            {
                m_dwellSeconds[playerId] = 0;
                m_screenTimerService.hide(player); // выехал из зоны — бар гаснет (вернётся при заезде)
            }
        }
        else
        {
            m_dwellSeconds[playerId] = 0;
            m_screenTimerService.hide(player); // едет между чекпоинтами — активного окна нет
        }
    }
    else
    {
        // Вышел из автобуса (или сел в другой) — окно возврата за руль.
        m_dwellSeconds[playerId] = 0;
        const int away = ++m_exitSeconds[playerId];
        if (away >= RETURN_SECONDS)
        {
            dismiss(player, "Вы не вернулись за руль автобуса вовремя — вы уволены", ERROR_COLOUR);
            return; // смена снята (GUI-таймер погашен в teardownShift)
        }
        // +1: первый тик вне руля (away==1) показывает полное RETURN_SECONDS, как окна
        // посадки/остановки выставляют полный остаток в момент события.
        m_screenTimerService.show(player, RETURN_SECONDS - away + 1, RETURN_TIMER_LABEL);
    }
}

// ------------------------------------------------------------------ увольнение

void BusJobSystem::failBoarding(IPlayer &player)
{
    const int playerId = player.getID();

    m_checkpointService.clearRaceForPlayer(player);
    m_waypointService.clearFor(player); // снять маркер автобуса — не залипает в очереди
    m_screenTimerService.hide(player);  // окно посадки закрыто — гасим таймер
    const int vid = m_busJobService.vehicleIdOf(playerId);

    m_busJobService.requeueTail(playerId); // освобождает площадку + phase Queued + хвост очереди
    if (vid >= 0)
    {
        // Подавали ЕМУ — значит и убираем: pre-stock в депо нет, брошенный автобус
        // просто занимал бы площадку и парк.
        m_vehicleService.destroy(vid);
    }
    clearCounters(playerId);

    player.sendClientMessage(ERROR_COLOUR,
                             u("Вы не успели занять автобус — он снят. Вы возвращены в конец очереди"));
    m_screenNoticeService.show(player, "boarding failed - back to queue", FAIL_POPUP_TIME, ERROR_POPUP_COLOUR);
    // Продвижение очереди — на следующем тике депо.
}

void BusJobSystem::dismiss(IPlayer &player, const std::string &reason, const Colour &colour)
{
    if (!m_busJobService.isWorking(player.getID()))
    {
        return; // защитно
    }
    teardownShift(player);
    player.sendClientMessage(colour, u(reason));
    // Продвижение очереди/пере-сток — на следующем тике депо.
}

void BusJobSystem::teardownShift(IPlayer &player)
{
    const int playerId = player.getID();

    m_checkpointService.clearRaceForPlayer(player);
    m_waypointService.clearFor(player); // снять маркер автобуса на любом конце смены (Reserved)
    m_screenTimerService.hide(player);  // любое завершение смены — гасим GUI-таймер

    const int vid = m_busJobService.vehicleIdOf(playerId);

    // Автобус всегда деспавним: он личный, подавался этому работнику. Оставлять его на
    // площадке было бы возвратом к pre-stock — машина стояла бы ничья и занимала и
    // площадку, и место в парке.
    // Порядок: сперва снять состояние сервиса (площадка/очередь), ПОТОМ destroy.
    m_busJobService.endShift(playerId);
    if (vid >= 0)
    {
        m_vehicleService.destroy(vid);
    }
    clearCounters(playerId);
    m_navLockService.release(playerId); // конец смены — снять лок навигации (общий путь)
}

// ------------------------------------------------------------------ лайфцикл сессии

void BusJobSystem::onSessionEnd(IPlayer &player)
{
    const int playerId = player.getID();

    if (m_busJobService.isWorking(playerId))
    {
        const bool wasQueued = m_busJobService.phaseOf(playerId) == BusJobService::Phase::Queued;
        teardownShift(player); // без сообщения — игрок уходит
        if (wasQueued)
        {
            notifyQueueShift(); // ушёл из очереди — стоявшие позади подвинулись
        }
        // Освободившийся автобус/промоушен — на следующем тике депо.
    }
    m_busWalletService.reset(playerId); // teardown ТОЛЬКО кэша — баланс остаётся в БД
}

void BusJobSystem::onPlayerDeath(IPlayer &player)
{
    if (!m_busJobService.isWorking(player.getID()))
    {
        return; // не в смене — смерть работы не касается
    }
    dismiss(player, "Вы погибли — смена водителя автобуса завершена", ERROR_COLOUR);
}

void BusJobSystem::loadWallet(IPlayer &player, const PlayerSessionService::Session &session)
{
    DatabaseManager::selectQuery<std::int64_t>(
        [accountId = session.accountId](mysqlx::Schema schema) -> std::int64_t
        {
            mysqlx::RowResult result = schema.getTable("bus_wallet")
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
            m_busWalletService.load(playerId, balance);
            m_jobWalletService.notifyLoaded(playerId); // общий список знает, что баланс готов
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "BusJobSystem: failed to load bus wallet: " + error);
        });
}

// ------------------------------------------------------------------ helpers

bool BusJobSystem::drivingOwnBus(int playerId) const
{
    const int vid = m_busJobService.vehicleIdOf(playerId);
    return vid >= 0 && m_vehicleService.getDriver(vid) == playerId;
}

bool BusJobSystem::busOnSpot(int vid, int spot) const
{
    if (vid < 0 || spot < 0 || spot >= BusJobService::SLOT_COUNT)
    {
        return false;
    }
    IVehicle *bus = m_vehicleService.get(vid);
    if (!bus)
    {
        return false; // автобус исчез — не «на площадке»
    }
    return distanceSq2D(bus->getPosition(), SLOT_POS[spot]) <= SPOT_RADIUS * SPOT_RADIUS;
}

bool BusJobSystem::spotClear(int spot) const
{
    if (spot < 0 || spot >= BusJobService::SLOT_COUNT)
    {
        return false; // вне диапазона -> считаем занятой (не спавним)
    }
    // Многопробная занятость (центр + вперёд/назад по SLOT_ANGLE), как
    // ParkingSystem::isSpotFree: длинный автобус на площадке детектится телом, а не
    // только origin'ом; соседние площадки (по X) под пробами (по Y) ложно не заняты.
    const Vector3 &c = SLOT_POS[spot];
    const Vector3 forward = Geometry::forwardOf(c, SLOT_ANGLE, SPOT_PROBE_OFFSET);
    const Vector3 back = Geometry::backOf(c, SLOT_ANGLE, SPOT_PROBE_OFFSET);
    return !m_vehicleService.anyVehicleNear(c, SPOT_OCCUPIED_RADIUS) &&
           !m_vehicleService.anyVehicleNear(forward, SPOT_OCCUPIED_RADIUS) &&
           !m_vehicleService.anyVehicleNear(back, SPOT_OCCUPIED_RADIUS);
}

void BusJobSystem::clearCounters(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_reserveSeconds[playerId] = 0;
    m_exitSeconds[playerId] = 0;
    m_dwellSeconds[playerId] = 0;
    m_idleSeconds[playerId] = 0;
}
