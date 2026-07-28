#include "Systems/MedicJobSystem/MedicJobSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <cstdint>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <string>

namespace
{
// Игровые цвета сообщений (как в остальных работах).
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Пикап трудоустройства у больницы (info-икона, сервис-точка, подбор по касанию) +
// иконка на миникарте (22 — больница). Координаты — замеры владельца.
const Vector3 EMPLOY_PICKUP_POS{1183.6260f, -1332.1185f, 13.5814f};
constexpr int PICKUP_MODEL = 1239;
constexpr PickupType PICKUP_TYPE = 1;
constexpr int JOB_MAP_ICON = 22;

// Скорая (модель 416) и две точки спавна у больницы — замеры владельца вместе с
// углом и цветами.
constexpr int AMBULANCE_MODEL = 416;
constexpr int AMBULANCE_COLOUR_1 = 1;
constexpr int AMBULANCE_COLOUR_2 = 3;
struct SpawnSpot
{
    Vector3 position;
    float angle;
};
const SpawnSpot SPAWN_SPOT[MedicJobService::SPOT_COUNT] = {
    {{1180.7723f, -1338.7773f, 13.8991f}, 271.2566f},
    {{1180.6863f, -1308.6395f, 13.8431f}, 269.4227f},
};

// «Машина ещё на точке спавна»: XY-дистанция ОРИГИНА до точки. Отъехал дальше —
// точка свободна для следующего из очереди.
constexpr float SPOT_RADIUS = 8.0f;

// Форма врача: пул скинов по полу аккаунта (замеры владельца), выбор случайный.
const int UNIFORM_MALE[] = {274, 275, 276};
const int UNIFORM_FEMALE[] = {69};

// Радиус, в котором работает /med — вокруг СКОРОЙ врача. Требуется от ОБОИХ: врач
// лечит у машины, пациента к машине приводят. Иначе «лечение» шло бы через карту.
constexpr float HEAL_RADIUS = 5.0f;

// Баланс.
constexpr std::int64_t PAY_PER_HEAL = 100; // $ за каждого вылеченного
constexpr int BOARDING_SECONDS = 30;       // окно «сесть и отъехать с точки спавна»

// Экранные попапы — ТОЛЬКО English (конвенция проекта).
constexpr Milliseconds BOARDING_POPUP_TIME{4000};
constexpr Milliseconds CREDIT_POPUP_TIME{2500};
constexpr Milliseconds FAIL_POPUP_TIME{3000};
const Colour NEUTRAL_POPUP_COLOUR{0xFF, 0xFF, 0xFF, 0xFF};
const Colour CREDIT_POPUP_COLOUR{0x90, 0xEE, 0x90, 0xFF};
const Colour ERROR_POPUP_COLOUR{0xFF, 0x5A, 0x5A, 0xFF};

const char *const BOARDING_TIMER_LABEL = "AMBULANCE";

// Горизонтальная (XY) дистанция² — Z у больницы одинаковый, а этажи тут не при чём.
float distanceSq2D(const Vector3 &a, const Vector3 &b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

TimePoint now()
{
    return std::chrono::steady_clock::now();
}
} // namespace

MedicJobSystem::MedicJobSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_medicJobService(serviceRegister.getService<MedicJobService>()),
      m_medicWalletService(serviceRegister.getService<MedicWalletService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_pickupService(serviceRegister.getService<PickupService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_healthService(serviceRegister.getService<PlayerHealthService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_skinService(serviceRegister.getService<PlayerSkinService>()),
      m_personalSkinService(serviceRegister.getService<PlayerPersonalSkinService>()),
      m_timers(serviceRegister.getService<TimerService>()),
      m_screenNoticeService(serviceRegister.getService<ScreenNoticeService>()),
      m_screenTimerService(serviceRegister.getService<ScreenTimerService>()),
      m_mapIconService(serviceRegister.getService<MapIconService>()),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>()),
      m_navLockService(serviceRegister.getService<NavigationLockService>()),
      m_jobWalletService(serviceRegister.getService<JobWalletService>()), m_rng(std::random_device{}())
{
    // Загрузка персистентного кошелька врача по старту сессии (serial-guard).
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            loadWallet(player, session);
        });

    // Конец сессии в ЛЮБОЙ фазе: тихий teardown смены (деспавн скорой, снятие точки/
    // очереди, возврат личного скина) + сброс ОЗУ-кэша кошелька и кулдауна пациента.
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            onSessionEnd(player);
        });

    // Смерть в смене серверно-авторитетна — увольнение (правило работ проекта).
    m_healthService.subscribeDeath(
        [this](IPlayer &player)
        {
            onPlayerDeath(player);
        });

    // Гейт водителя: за руль скорой — только её работник. Чужие Owner::Work
    // (автобусы, грузовики, дев-машины) пропускаем.
    m_vehicleService.subscribeDriverGate([this](IPlayer &player, IVehicle &vehicle)
                                         { return onDriverGate(player, vehicle); });

    PlayerCommandService &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add(
        "med", {{PlayerCommandService::Param::Int, "id игрока"}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            onMedCommand(player, args.getInt(0));
        },
        {}, "вылечить игрока рядом со скорой (врач)", PlayerCommandService::HelpCategory::Economy);

    // Универсальный выход с работы (/stopjob) — увольняет наш же обработчик пикапа.
    serviceRegister.getService<JobDismissService>().registerJob(
        "врач",
        "Скорая будет снята, форма сменится обратно на вашу одежду. Заработок в кошельке врача сохранится, его "
        "можно забрать у пикапа больницы.",
        [this](int playerId)
        {
            return m_medicJobService.isWorking(playerId);
        },
        [this](IPlayer &player)
        {
            onFinishWork(player);
        });

    // Кошелёк этой работы — в справочный список (/jobwallet). Только баланс: выдача
    // остаётся на пикапе работы, туда за деньгами и едут.
    m_jobWalletService.registerWallet("врач", "пикап работы у больницы",
                                      [this](int playerId)
                                      {
                                          return m_medicWalletService.balanceOf(playerId);
                                      });
}

void MedicJobSystem::initialize(IComponentList * /*components*/)
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

    // Один общий per-second таймер больницы на весь сервер: НИКОГДА не отменяется
    // (в т.ч. из своего колбэка) — ведёт окна посадки и освобождение точек спавна.
    m_hospitalTimer = m_timers.setInterval(Milliseconds{1000},
                                           [this]()
                                           {
                                               onHospitalTick();
                                           });
}

// ------------------------------------------------------------------ пикап + диалог

void MedicJobSystem::onPickup(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId))
    {
        return;
    }

    // Все пункты видны ВСЕГДА (правило проекта): гейт — в обработчике по клику.
    const std::string body = "Начать работу\nЗавершить работу\nЗабрать деньги\nИнформация";
    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Работа — врач", body, "Выбрать", "Закрыть"),
                         [this, playerId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *worker = m_core.getPlayers().get(playerId);
                             if (!worker || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             switch (listItem)
                             {
                             case 0:
                                 onStartWork(*worker);
                                 break;
                             case 1:
                                 onFinishWork(*worker);
                                 break;
                             case 2:
                                 onWithdrawMoney(*worker);
                                 break;
                             case 3:
                                 showInfo(*worker);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void MedicJobSystem::onStartWork(IPlayer &player)
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
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из транспорта, чтобы устроиться врачом"));
        return;
    }
    if (m_medicJobService.isWorking(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы уже работаете врачом"));
        return;
    }
    // Взаимное исключение работ: чекпоинт-слот и лок навигации принадлежат ТЕКУЩЕЙ
    // смене. isWorking(врач) выше уже ложно, значит держатель лока — ДРУГАЯ работа.
    if (m_navLockService.isLocked(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Нельзя устроиться врачом: {}", m_navLockService.lockReason(playerId))));
        return;
    }

    const MedicJobService::StartOutcome outcome = m_medicJobService.startWork(playerId);
    if (outcome.result == MedicJobService::StartResult::AlreadyWorking)
    {
        return; // гонка кликов
    }

    // Лок навигации на ВСЮ смену; освобождается на любом её конце.
    m_navLockService.acquire(playerId, "идёт смена врача");
    applyUniform(player); // форма выдаётся сразу, ещё до машины

    if (outcome.result == MedicJobService::StartResult::Queued)
    {
        player.sendClientMessage(
            INFO_COLOUR,
            u(fmt::format("Обе точки выезда заняты. Вы в очереди на скорую, место {}", outcome.queuePosition)));
        return;
    }
    onSpotGranted(player);
}

void MedicJobSystem::onFinishWork(IPlayer &player)
{
    const int playerId = player.getID();
    const MedicJobService::Phase phase = m_medicJobService.phaseOf(playerId);

    if (phase == MedicJobService::Phase::NotWorking)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы сейчас не работаете врачом"));
        return;
    }
    if (phase == MedicJobService::Phase::Queued)
    {
        m_medicJobService.endShift(playerId);
        restoreOwnSkin(player);
        m_navLockService.release(playerId);
        m_boardingSeconds[playerId] = 0;
        player.sendClientMessage(INFO_COLOUR, u("Вы вышли из очереди на скорую"));
        notifyQueueShift();
        return;
    }
    dismiss(player, "Смена окончена. Заработок сохранён в кошельке врача — заберите через «Забрать деньги»",
            INFO_COLOUR);
}

void MedicJobSystem::onWithdrawMoney(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId))
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
    if (m_medicWalletService.balanceOf(playerId) <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Забирать нечего"));
        return;
    }

    const std::int64_t amount = m_medicWalletService.withdraw(playerId, session->accountId);
    if (amount <= 0)
    {
        return; // гонка кликов — уже забрано
    }
    m_moneyService.giveMoney(player, static_cast<unsigned long long>(amount));
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Вы забрали ${} из кошелька врача", amount)));
}

void MedicJobSystem::showInfo(IPlayer &player)
{
    const int playerId = player.getID();

    std::string body = "Поле\tЗначение\n";
    body += "Суть работы\tЛечить игроков у своей скорой командой /med [id]\n";
    body += "Вступительный взнос\tнет\n";
    body += fmt::format("Ставка\t${} за каждого вылеченного\n", PAY_PER_HEAL);
    body += fmt::format("Радиус лечения\t{:.0f} м от вашей скорой — и врач, и пациент\n", HEAL_RADIUS);
    body += fmt::format("Повторное лечение\tодного игрока — не чаще раза в {} минут\n",
                        static_cast<int>(MedicJobService::HEAL_COOLDOWN.count()));
    body += fmt::format("Точек выезда\t{}; заняты — ждёте в очереди\n", MedicJobService::SPOT_COUNT);
    body += fmt::format("Выезд\t{} секунд сесть в скорую и отъехать с точки\n", BOARDING_SECONDS);
    body += "Форма\tвыдаётся на смену, своя одежда возвращается при увольнении\n";
    body += "Выплата\tкопится в кошельке врача; на руки — через «Забрать деньги»\n";

    std::string status;
    switch (m_medicJobService.phaseOf(playerId))
    {
    case MedicJobService::Phase::NotWorking:
        status = "не работаете";
        break;
    case MedicJobService::Phase::Queued:
        status = fmt::format("в очереди на скорую, место {}", m_medicJobService.queuePositionOf(playerId));
        break;
    case MedicJobService::Phase::Boarding:
        status = "скорая подана — сядьте и отъезжайте с точки";
        break;
    case MedicJobService::Phase::Working:
        status = "на смене";
        break;
    }
    body += fmt::format("Ваш статус\t{}\n", status);
    body += fmt::format("В кошельке врача\t${}", m_medicWalletService.balanceOf(playerId));

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, "Работа врачом", body, "Назад", ""),
                         [](DialogResponse, int, StringView)
                         {
                         });
}

// ------------------------------------------------------------------ гейт водителя

bool MedicJobSystem::onDriverGate(IPlayer &player, IVehicle &vehicle)
{
    const int vid = vehicle.getID();
    // Дешёвый фильтр горячего пути: наши скорые — всегда Owner::Work. Прочий
    // транспорт (в т.ч. автобусы и грузовики — тоже Work) распознаётся ниже по
    // нашему стейту, чужой пропускаем.
    if (m_vehicleService.getOwner(vid) != VehicleService::Owner::Work)
    {
        return true;
    }

    const int worker = m_medicJobService.workerOfVehicle(vid);
    if (worker < 0)
    {
        return true; // не наша машина — гейт не наш
    }
    if (worker == player.getID())
    {
        m_waypointService.clearFor(player); // сел в свою — маркер больше не нужен
        return true;
    }
    player.sendClientMessage(ERROR_COLOUR, u("Это скорая помощь — за руль пускают только назначенного врача"));
    return false;
}

// ------------------------------------------------------------------ очередь и выдача

void MedicJobSystem::onSpotGranted(IPlayer &player)
{
    const int playerId = player.getID();
    const int spot = m_medicJobService.spotOf(playerId);
    if (spot < 0 || spot >= MedicJobService::SPOT_COUNT)
    {
        return;
    }

    IVehicle *ambulance =
        m_vehicleService.create(AMBULANCE_MODEL, SPAWN_SPOT[spot].position, SPAWN_SPOT[spot].angle,
                                AMBULANCE_COLOUR_1, AMBULANCE_COLOUR_2, VehicleService::Owner::Work, -1);
    if (!ambulance)
    {
        LogManager::log(Error, "MedicJobSystem: vehicle pool full, ambulance not spawned");
        // Точку не держим: вернуть работника в очередь честнее, чем оставить его
        // с занятой точкой и без машины.
        m_medicJobService.requeueTail(playerId);
        player.sendClientMessage(ERROR_COLOUR, u("Скорую сейчас не подать. Вы возвращены в очередь"));
        return;
    }
    m_vehicleService.setInfiniteFuel(*ambulance, true); // рабочий транспорт — бак не расходуется
    m_medicJobService.setVehicle(playerId, ambulance->getID());

    m_boardingSeconds[playerId] = 0;
    m_waypointService.showFor(player, *ambulance); // красный маркер на поданную скорую
    m_screenTimerService.show(player, BOARDING_SECONDS, BOARDING_TIMER_LABEL);
    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Скорая подана и отмечена маркером. У вас {} секунд сесть за руль и "
                                           "отъехать — точка выезда нужна другим",
                                           BOARDING_SECONDS)));
    m_screenNoticeService.show(player, "ambulance ready - get in and drive off", BOARDING_POPUP_TIME,
                               NEUTRAL_POPUP_COLOUR);
}

void MedicJobSystem::pumpQueue()
{
    bool promoted = false;
    for (;;)
    {
        const MedicJobService::Promotion promotion = m_medicJobService.promoteQueue();
        if (promotion.playerId < 0)
        {
            break; // очередь пуста / свободных точек нет
        }
        IPlayer *next = m_core.getPlayers().get(promotion.playerId);
        if (!next)
        {
            // Продвинутый работник пропал — снять смену (машину ему не спавнили).
            m_medicJobService.endShift(promotion.playerId);
            m_boardingSeconds[promotion.playerId] = 0;
            m_navLockService.release(promotion.playerId);
            continue;
        }
        onSpotGranted(*next);
        promoted = true;
    }
    if (promoted)
    {
        notifyQueueShift();
    }
}

void MedicJobSystem::notifyQueueShift()
{
    const std::vector<int> queued = m_medicJobService.queuedPlayers();
    for (int i = 0; i < static_cast<int>(queued.size()); ++i)
    {
        IPlayer *waiting = m_core.getPlayers().get(queued[i]);
        if (!waiting)
        {
            continue;
        }
        if (i == 0)
        {
            waiting->sendClientMessage(INFO_COLOUR, u("Очередь на скорую продвинулась — вы следующий"));
        }
        else
        {
            waiting->sendClientMessage(INFO_COLOUR,
                                       u(fmt::format("Очередь на скорую продвинулась, ваше место {}", i + 1)));
        }
    }
}

void MedicJobSystem::failBoarding(IPlayer &player)
{
    const int playerId = player.getID();
    const int vid = m_medicJobService.vehicleIdOf(playerId);

    m_waypointService.clearFor(player);
    m_screenTimerService.hide(player);
    m_medicJobService.requeueTail(playerId); // снять точку + phase Queued + хвост очереди
    if (vid >= 0)
    {
        m_vehicleService.destroy(vid); // точка выезда не может стоять занятой
    }
    m_boardingSeconds[playerId] = 0;

    player.sendClientMessage(
        ERROR_COLOUR, u("Вы не отъехали от больницы вовремя — скорая снята. Вы возвращены в конец очереди"));
    m_screenNoticeService.show(player, "too slow - back to queue", FAIL_POPUP_TIME, ERROR_POPUP_COLOUR);
    notifyQueueShift();
}

// ------------------------------------------------------------------ лечение

void MedicJobSystem::onMedCommand(IPlayer &medic, int targetId)
{
    const int medicId = medic.getID();

    if (m_medicJobService.phaseOf(medicId) != MedicJobService::Phase::Working)
    {
        medic.sendClientMessage(ERROR_COLOUR, u("Лечить может только врач на смене"));
        return;
    }

    Vector3 ambulance;
    if (!ambulancePosition(medicId, ambulance))
    {
        dismiss(medic, "Скорая пропала — смена окончена", ERROR_COLOUR);
        return;
    }

    IPlayer *patient = m_core.getPlayers().get(targetId);
    if (!patient || targetId == medicId)
    {
        medic.sendClientMessage(ERROR_COLOUR, u("Игрок не найден"));
        return;
    }

    // Дистанции — по ПРИНЯТЫМ сервером позициям: «я рядом» подделать нельзя.
    // Требуем близость к скорой от ОБОИХ: врач лечит у машины, пациента к машине
    // приводят. Иначе врач стоял бы у скорой и лечил через полкарты.
    constexpr float radiusSq = HEAL_RADIUS * HEAL_RADIUS;
    if (distanceSq2D(m_locationService.getPosition(medicId), ambulance) > radiusSq)
    {
        medic.sendClientMessage(ERROR_COLOUR,
                                u(fmt::format("Лечить можно только у своей скорой (не дальше {:.0f} м)", HEAL_RADIUS)));
        return;
    }
    if (distanceSq2D(m_locationService.getPosition(targetId), ambulance) > radiusSq)
    {
        medic.sendClientMessage(ERROR_COLOUR, u("Пациент слишком далеко от скорой — подведите его к машине"));
        return;
    }
    if (m_locationService.getVirtualWorld(targetId) != m_locationService.getVirtualWorld(medicId) ||
        m_locationService.getInterior(targetId) != m_locationService.getInterior(medicId))
    {
        medic.sendClientMessage(ERROR_COLOUR, u("Пациент слишком далеко от скорой — подведите его к машине"));
        return;
    }

    if (!m_healthService.isAlive(targetId))
    {
        medic.sendClientMessage(ERROR_COLOUR, u("Этому игроку врач уже не поможет"));
        return;
    }
    // Здоровому лечение не нужно — иначе это выплата ни за что.
    if (m_healthService.getHealth(targetId) >= PlayerHealthService::MAX_HEALTH)
    {
        medic.sendClientMessage(ERROR_COLOUR, u("Этот игрок здоров — лечить нечего"));
        return;
    }

    const TimePoint timeNow = now();
    const int cooldownLeft = m_medicJobService.healCooldownLeft(targetId, timeNow);
    if (cooldownLeft > 0)
    {
        medic.sendClientMessage(
            ERROR_COLOUR, u(fmt::format("Этого игрока недавно лечили. Повторно — через {} мин {} сек",
                                        cooldownLeft / 60, cooldownLeft % 60)));
        return;
    }

    m_healthService.setHealth(*patient, PlayerHealthService::MAX_HEALTH);
    m_medicJobService.markHealed(targetId, timeNow);

    const PlayerSessionService::AccountId accountId = m_sessionService.getAccountId(medicId);
    m_medicWalletService.add(medicId, accountId, PAY_PER_HEAL);

    // Ники обеих сторон — клиентский текст: цветокоды в сообщение не пускаем.
    const StringView medicName = medic.getName();
    const std::string medicSafe = Encoding::neutralizeColorCodes(std::string_view(medicName.data(), medicName.size()));
    const StringView patientName = patient->getName();
    const std::string patientSafe =
        Encoding::neutralizeColorCodes(std::string_view(patientName.data(), patientName.size()));

    medic.sendClientMessage(INFO_COLOUR, u(fmt::format("Вы вылечили {}[{}]. Начислено ${}", patientSafe, targetId,
                                                       PAY_PER_HEAL)));
    patient->sendClientMessage(INFO_COLOUR, u(fmt::format("Врач {}[{}] вылечил вас", medicSafe, medicId)));
    m_screenNoticeService.show(medic, fmt::format("+${}, wallet ${}", PAY_PER_HEAL,
                                                  m_medicWalletService.balanceOf(medicId)),
                               CREDIT_POPUP_TIME, CREDIT_POPUP_COLOUR);
}

// ------------------------------------------------------------------ форма

void MedicJobSystem::applyUniform(IPlayer &player)
{
    const int playerId = player.getID();
    // Пол — из сессии (player.sex аккаунта), а не из текущего скина: скин игрок мог
    // сменить, пол аккаунта — нет.
    const bool female = m_sessionService.isFemale(playerId);
    const int *pool = female ? UNIFORM_FEMALE : UNIFORM_MALE;
    const int poolSize = female ? static_cast<int>(std::size(UNIFORM_FEMALE)) : static_cast<int>(std::size(UNIFORM_MALE));

    std::uniform_int_distribution<int> dist(0, poolSize - 1);
    const int skin = pool[dist(m_rng)];

    // Форма — БАЗА, а не временный скин: временный живёт до ближайшего респавна, и
    // первая же смерть вернула бы врача в гражданском. Смена базы пересобирает
    // спавн-инфо через наблюдателя PlayerSkinService.
    m_skinService.setSkin(player, skin);
}

void MedicJobSystem::restoreOwnSkin(IPlayer &player)
{
    // Личный скин аккаунта живёт в PlayerPersonalSkinService и сменой формы не
    // затрагивался — возвращаем его же (внутри фолбэк на валидный дефолт).
    m_skinService.setSkin(player, m_personalSkinService.getSkin(player.getID()));
}

// ------------------------------------------------------------------ таймер больницы

void MedicJobSystem::onHospitalTick()
{
    for (IPlayer *player : m_core.getPlayers().entries())
    {
        if (!player)
        {
            continue;
        }
        const int playerId = player->getID();
        if (!validPlayerId(playerId))
        {
            continue;
        }
        const MedicJobService::Phase phase = m_medicJobService.phaseOf(playerId);
        if (phase == MedicJobService::Phase::Boarding || phase == MedicJobService::Phase::Working)
        {
            tickWorker(*player);
        }
    }
    pumpQueue(); // освободившиеся точки — следующим из очереди
}

void MedicJobSystem::tickWorker(IPlayer &player)
{
    const int playerId = player.getID();

    // Смена держит ЛИЧНУЮ скорую: пропала (уничтожена/деспавн) -> увольнение штатно.
    const int vid = m_medicJobService.vehicleIdOf(playerId);
    if (vid >= 0 && !m_vehicleService.get(vid))
    {
        dismiss(player, "Скорая пропала — смена окончена", ERROR_COLOUR);
        return;
    }

    if (m_medicJobService.phaseOf(playerId) != MedicJobService::Phase::Boarding)
    {
        return; // на смене окон нет: врач легально ходит пешком у своей машины
    }

    const int spot = m_medicJobService.spotOf(playerId);
    // Окно закрывает ОТЪЕЗД, а не посадка: точка выезда нужна следующему из очереди,
    // и сидящий на ней врач блокировал бы её всю смену.
    if (!ambulanceOnSpot(vid, spot))
    {
        m_medicJobService.completeBoarding(playerId);
        m_boardingSeconds[playerId] = 0;
        m_waypointService.clearFor(player);
        m_screenTimerService.hide(player);
        player.sendClientMessage(INFO_COLOUR,
                                 u(fmt::format("Вы на смене. Лечите игроков у своей скорой: /med [id], ${} за "
                                               "каждого",
                                               PAY_PER_HEAL)));
        m_screenNoticeService.show(player, "on duty - heal players at your ambulance", BOARDING_POPUP_TIME,
                                   NEUTRAL_POPUP_COLOUR);
        return;
    }

    const int elapsed = ++m_boardingSeconds[playerId];
    if (elapsed >= BOARDING_SECONDS)
    {
        failBoarding(player);
        return;
    }
    m_screenTimerService.show(player, BOARDING_SECONDS - elapsed, BOARDING_TIMER_LABEL);
}

// ------------------------------------------------------------------ увольнение

void MedicJobSystem::dismiss(IPlayer &player, const std::string &reason, const Colour &colour)
{
    if (!m_medicJobService.isWorking(player.getID()))
    {
        return; // защитно
    }
    teardownShift(player);
    player.sendClientMessage(colour, u(reason));
}

void MedicJobSystem::teardownShift(IPlayer &player)
{
    const int playerId = player.getID();

    m_waypointService.clearFor(player);
    m_screenTimerService.hide(player);
    restoreOwnSkin(player);

    const int vid = m_medicJobService.vehicleIdOf(playerId);
    m_medicJobService.endShift(playerId);
    if (vid >= 0)
    {
        m_vehicleService.destroy(vid); // личная скорая не остаётся хламом
    }
    if (validPlayerId(playerId))
    {
        m_boardingSeconds[playerId] = 0;
    }
    m_navLockService.release(playerId); // конец смены — снять лок навигации
    pumpQueue();                        // освободилась точка — отдать её очереди
}

// ------------------------------------------------------------------ лайфцикл сессии

void MedicJobSystem::onSessionEnd(IPlayer &player)
{
    const int playerId = player.getID();

    if (m_medicJobService.isWorking(playerId))
    {
        const bool wasQueued = m_medicJobService.phaseOf(playerId) == MedicJobService::Phase::Queued;
        teardownShift(player); // без сообщения — игрок уходит
        if (wasQueued)
        {
            notifyQueueShift();
        }
    }
    m_medicWalletService.reset(playerId);   // teardown ТОЛЬКО кэша — баланс в БД остаётся
    m_medicJobService.resetPatient(playerId); // чужой кулдаун лечения не наследуем
}

void MedicJobSystem::onPlayerDeath(IPlayer &player)
{
    if (!m_medicJobService.isWorking(player.getID()))
    {
        return; // не в смене — смерть работы не касается
    }
    dismiss(player, "Вы погибли — смена врача завершена", ERROR_COLOUR);
}

void MedicJobSystem::loadWallet(IPlayer &player, const PlayerSessionService::Session &session)
{
    DatabaseManager::selectQuery<std::int64_t>(
        [accountId = session.accountId](mysqlx::Schema schema) -> std::int64_t
        {
            mysqlx::RowResult result = schema.getTable("medic_wallet")
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
            m_medicWalletService.load(playerId, balance);
            m_jobWalletService.notifyLoaded(playerId); // общий список знает, что баланс готов
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "MedicJobSystem: failed to load medic wallet: " + error);
        });
}

// ------------------------------------------------------------------ helpers

bool MedicJobSystem::ambulanceOnSpot(int vehicleId, int spot) const
{
    if (vehicleId < 0 || spot < 0 || spot >= MedicJobService::SPOT_COUNT)
    {
        return false;
    }
    IVehicle *ambulance = m_vehicleService.get(vehicleId);
    if (!ambulance)
    {
        return false;
    }
    return distanceSq2D(ambulance->getPosition(), SPAWN_SPOT[spot].position) <= SPOT_RADIUS * SPOT_RADIUS;
}

bool MedicJobSystem::ambulancePosition(int playerId, Vector3 &out) const
{
    IVehicle *ambulance = m_vehicleService.get(m_medicJobService.vehicleIdOf(playerId));
    if (!ambulance)
    {
        return false;
    }
    out = ambulance->getPosition();
    return true;
}
