#include "Systems/TaxiJobSystem/TaxiJobSystem.h"

#include "Log/LogManager.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/Geometry/Geometry.h"
#include <cstdint>
#include <fmt/format.h>
#include <string>

namespace
{
// Игровые цвета сообщений (как в остальных работах).
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Пикап трудоустройства у таксопарка (info-икона, подбор по касанию) + иконка на
// миникарте (42 — такси). Координаты и id иконки — от владельца.
const Vector3 EMPLOY_PICKUP_POS{1076.2310f, -1779.8088f, 13.5837f};
constexpr int PICKUP_MODEL = 1239;
constexpr PickupType PICKUP_TYPE = 1;
constexpr int JOB_MAP_ICON = 42;

// Такси (модель 420) и семь точек спавна — замеры владельца вместе с углом и цветами.
constexpr int TAXI_MODEL = 420;
constexpr int TAXI_COLOUR_1 = 6;
constexpr int TAXI_COLOUR_2 = 1;
struct SpawnSpot
{
    Vector3 position;
    float angle;
};
const SpawnSpot SPAWN_SPOT[TaxiJobService::SPOT_COUNT] = {
    {{1062.4664f, -1775.6044f, 13.1238f}, 270.6901f}, {{1062.5526f, -1769.6901f, 13.1457f}, 269.6044f},
    {{1062.5842f, -1763.8137f, 13.1705f}, 270.3944f}, {{1062.6049f, -1757.9246f, 13.1958f}, 270.1638f},
    {{1062.7031f, -1751.9862f, 13.2234f}, 270.6616f}, {{1062.6295f, -1746.1515f, 13.2359f}, 270.7126f},
    {{1062.6852f, -1740.2670f, 13.2501f}, 270.0336f},
};

// «Машина ещё на точке спавна»: XY-дистанция ОРИГИНА до точки. Точки стоят в ряд с
// шагом ~5.9 м, поэтому радиус меньше шага — иначе соседние точки считались бы занятыми.
constexpr float SPOT_RADIUS = 4.0f;

// Физическая занятость точки ЛЮБОЙ машиной (не только нашей).
//
// anyVehicleNear сравнивает ОРИГИНЫ машин, а не их габариты: чужая машина перекрывает
// место, стоя оригином за 3-4 м от центра. Поэтому покрытие считаем от размеров ДВУХ
// машин — подаваемой и мешающей (обе ~4.5 м в длину, ~2 м в ширину): пересечение
// возможно, пока оригины ближе ~4.5 м вдоль кузова и ~2 м поперёк.
//
// Геометрия площадки: угол мест ~270°, то есть кузов лежит вдоль X, а соседние точки
// разнесены по Y на ~5.9 м. Пробы идут ВДОЛЬ X (к соседям не приближаются), поэтому
// оффсет вдоль оси можно брать щедрый; ограничен только РАДИУС — он же покрытие по Y,
// и должен остаться заметно меньше 5.9, иначе машина на соседнем месте считалась бы
// помехой. 3.5 держит этот зазор и ловит всё, что реально перекрывает место.
constexpr float SPOT_OCCUPIED_RADIUS = 3.5f;
constexpr float SPOT_PROBE_OFFSET = 3.0f;

// Радиус чекпоинта точки назначения (стандартный наземный, как у прочих работ).
constexpr float DESTINATION_CP_RADIUS = 4.0f;

// Потолок цены поездки: защита от опечатки в /taxipay и от «предложения» на
// астрономическую сумму. Проверка «у пассажира столько есть» — отдельно и всегда.
constexpr std::int64_t MAX_FARE = 1000000;

constexpr int BOARDING_SECONDS = 30; // окно «сесть и отъехать с точки спавна»

// Окно возврата за руль на смене: такси без водителя стоит брошенным посреди города,
// а оплаченный пассажир ждёт поездки, которой не будет. Не вернулся — увольнение, и
// депозит уходит обратно пассажиру.
constexpr int RETURN_SECONDS = 60;

// Экранные попапы — ТОЛЬКО English (конвенция проекта).
constexpr Milliseconds BOARDING_POPUP_TIME{4000};
constexpr Milliseconds CREDIT_POPUP_TIME{2500};
constexpr Milliseconds FAIL_POPUP_TIME{3000};
const Colour NEUTRAL_POPUP_COLOUR{0xFF, 0xFF, 0xFF, 0xFF};
const Colour CREDIT_POPUP_COLOUR{0x90, 0xEE, 0x90, 0xFF};
const Colour ERROR_POPUP_COLOUR{0xFF, 0x5A, 0x5A, 0xFF};

const char *const BOARDING_TIMER_LABEL = "TAXI";
const char *const RETURN_TIMER_LABEL = "RETURN";

// Горизонтальная (XY) дистанция² — точки спавна стоят на одной площадке.
float distanceSq2D(const Vector3 &a, const Vector3 &b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// Ник — клиентский текст: цветокоды в сообщение не пускаем.
std::string safeName(IPlayer &player)
{
    const StringView name = player.getName();
    return Encoding::neutralizeColorCodes(std::string_view(name.data(), name.size()));
}
} // namespace

TaxiJobSystem::TaxiJobSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_taxiJobService(serviceRegister.getService<TaxiJobService>()),
      m_placeCatalogService(serviceRegister.getService<PlaceCatalogService>()),
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
      m_navLockService(serviceRegister.getService<NavigationLockService>())
{
    // Клик по карте — единственный способ получить «метку» от клиента.
    listen(core.getPlayers().getPlayerClickDispatcher(), this);

    // Конец сессии в ЛЮБОЙ роли: у водителя — teardown смены (с возвратом депозита),
    // у пассажира — закрытие его поездки (тоже с возвратом).
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            onSessionEnd(player);
        });

    // Смерть серверно-авторитетна: водителя увольняем, пассажиру возвращаем депозит.
    m_healthService.subscribeDeath(
        [this](IPlayer &player)
        {
            onPlayerDeath(player);
        });

    m_vehicleService.subscribeDriverGate([this](IPlayer &player, IVehicle &vehicle)
                                         { return onDriverGate(player, vehicle); });

    PlayerCommandService &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add(
        "taxi", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            onTaxiCommand(player);
        },
        {}, "указать таксисту точку назначения (для пассажира)", PlayerCommandService::HelpCategory::Economy);

    commands.add(
        "taxipay", {{PlayerCommandService::Param::Int, "сумма"}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            onTaxiPayCommand(player, args.getInt(0));
        },
        {}, "назначить пассажиру цену поездки (таксист)", PlayerCommandService::HelpCategory::Economy);

    // Универсальный выход с работы (/stopjob) — увольняет наш же обработчик пикапа.
    serviceRegister.getService<JobDismissService>().registerJob(
        "таксист",
        "Такси будет снято. Если у вас сейчас оплаченный пассажир, его деньги вернутся ему — поездка не "
        "состоялась.",
        [this](int playerId)
        {
            return m_taxiJobService.isWorking(playerId);
        },
        [this](IPlayer &player)
        {
            onFinishWork(player);
        });
    // Вызовы такси по телефону идут тем, кто СЕЙЧАС на смене (фаза Working — машина
    // уже выехала с точки). Телефон про работу не знает: список приносим мы.
    serviceRegister.getService<PhoneService>().registerDispatch(
        PhoneService::Service::Taxi, "такси",
        [this](const PhoneService::WorkerVisitor &visit)
        {
            for (IPlayer *cabbie : m_core.getPlayers().entries())
            {
                if (cabbie && m_taxiJobService.phaseOf(cabbie->getID()) == TaxiJobService::Phase::Working)
                {
                    visit(cabbie->getID());
                }
            }
        });

    // Кошелька у таксиста нет: деньги за поездку приходят от ПАССАЖИРА и отдаются на
    // руки сразу по прибытии — копить их в депо-кошельке было бы неверно (пассажир
    // расстался с наличными мгновенно, водитель получил бы их с задержкой и в другом
    // месте). Поэтому в /jobwallet работа не регистрируется.
}

void TaxiJobSystem::initialize(IComponentList * /*components*/)
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

    // Один общий per-second таймер таксопарка: НИКОГДА не отменяется (в т.ч. из
    // своего колбэка) — ведёт окна выезда, освобождение точек и живость поездок.
    m_depotTimer = m_timers.setInterval(Milliseconds{1000},
                                        [this]()
                                        {
                                            onDepotTick();
                                        });
}

// ------------------------------------------------------------------ пикап + диалог

void TaxiJobSystem::onPickup(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId))
    {
        return;
    }

    // Все пункты видны ВСЕГДА (правило проекта): гейт — в обработчике по клику.
    const std::string body = "Начать работу\nЗавершить работу\nИнформация";
    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Работа — таксист", body, "Выбрать", "Закрыть"),
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
                                 showInfo(*worker);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void TaxiJobSystem::onStartWork(IPlayer &player)
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
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из транспорта, чтобы устроиться таксистом"));
        return;
    }
    if (m_taxiJobService.isWorking(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы уже работаете таксистом"));
        return;
    }
    // Взаимное исключение работ: чекпоинт-слот и лок навигации принадлежат ТЕКУЩЕЙ
    // смене. isWorking(таксист) выше уже ложно, значит держатель лока — ДРУГАЯ работа.
    if (m_navLockService.isLocked(playerId))
    {
        player.sendClientMessage(
            ERROR_COLOUR, u(fmt::format("Нельзя устроиться таксистом: {}", m_navLockService.lockReason(playerId))));
        return;
    }

    const TaxiJobService::StartOutcome outcome = m_taxiJobService.startWork(
        playerId,
        [this](int spot)
        {
            return spotClear(spot);
        });
    if (outcome.result == TaxiJobService::StartResult::AlreadyWorking)
    {
        return; // гонка кликов
    }

    // Лок навигации на ВСЮ смену; освобождается на любом её конце.
    m_navLockService.acquire(playerId, "идёт смена таксиста");

    if (outcome.result == TaxiJobService::StartResult::Queued)
    {
        // Не «все машины разобраны»: место могло быть и просто заставлено чужой
        // машиной — формулировка честна в обоих случаях.
        player.sendClientMessage(
            INFO_COLOUR,
            u(fmt::format("Свободных мест для подачи сейчас нет. Вы в очереди, место {}", outcome.queuePosition)));
        return;
    }
    onSpotGranted(player);
}

void TaxiJobSystem::onFinishWork(IPlayer &player)
{
    const int playerId = player.getID();
    const TaxiJobService::Phase phase = m_taxiJobService.phaseOf(playerId);

    if (phase == TaxiJobService::Phase::NotWorking)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы сейчас не работаете таксистом"));
        return;
    }
    if (phase == TaxiJobService::Phase::Queued)
    {
        m_taxiJobService.endShift(playerId);
        m_navLockService.release(playerId);
        m_boardingSeconds[playerId] = 0;
        player.sendClientMessage(INFO_COLOUR, u("Вы вышли из очереди на такси"));
        notifyQueueShift();
        return;
    }
    dismiss(player, "Смена таксиста окончена", INFO_COLOUR);
}

void TaxiJobSystem::showInfo(IPlayer &player)
{
    const int playerId = player.getID();

    std::string body = "Поле\tЗначение\n";
    body += "Суть работы\tВозить игроков туда, куда они попросят\n";
    body += "Вступительный взнос\tнет\n";
    body += "Цена поездки\tназначаете сами: /taxipay [сумма]\n";
    body += "Как это работает\tпассажир сел -> /taxi (он) -> /taxipay (вы) -> он соглашается\n";
    body += "Деньги\tсписываются с пассажира сразу, но приходят вам ТОЛЬКО по прибытии\n";
    body += "Не доехали\tденьги возвращаются пассажиру (вышли из игры, погибли и т.п.)\n";
    body += fmt::format("Машин в парке\t{}; разобраны — ждёте в очереди\n", TaxiJobService::SPOT_COUNT);
    body += fmt::format("Выезд\t{} секунд сесть в такси и отъехать с точки\n", BOARDING_SECONDS);

    std::string status;
    switch (m_taxiJobService.phaseOf(playerId))
    {
    case TaxiJobService::Phase::NotWorking:
        status = "не работаете";
        break;
    case TaxiJobService::Phase::Queued:
        status = fmt::format("в очереди на такси, место {}", m_taxiJobService.queuePositionOf(playerId));
        break;
    case TaxiJobService::Phase::Boarding:
        status = "такси подано — сядьте и отъезжайте с точки";
        break;
    case TaxiJobService::Phase::Working:
    {
        const TaxiJobService::Ride *ride = m_taxiJobService.rideOf(playerId);
        if (!ride || ride->passengerId < 0)
        {
            status = "на смене, пассажира нет";
        }
        else if (!ride->hasDestination)
        {
            status = fmt::format("пассажир id {} — ждёт, когда он укажет адрес", ride->passengerId);
        }
        else if (!ride->paid)
        {
            status = fmt::format("везёте id {} — цена не согласована", ride->passengerId);
        }
        else
        {
            status = fmt::format("везёте id {} за ${}", ride->passengerId, ride->fare);
        }
        break;
    }
    }
    body += fmt::format("Ваш статус\t{}", status);

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, "Работа таксистом", body, "Назад", ""),
                         [](DialogResponse, int, StringView)
                         {
                         });
}

// ------------------------------------------------------------------ гейт водителя

bool TaxiJobSystem::onDriverGate(IPlayer &player, IVehicle &vehicle)
{
    const int vid = vehicle.getID();
    // Дешёвый фильтр горячего пути: наши такси — всегда Owner::Work. Прочий транспорт
    // (автобусы, грузовики, скорые — тоже Work) распознаётся ниже по нашему стейту.
    if (m_vehicleService.getOwner(vid) != VehicleService::Owner::Work)
    {
        return true;
    }

    const int worker = m_taxiJobService.workerOfVehicle(vid);
    if (worker < 0)
    {
        return true; // не наша машина — гейт не наш
    }
    if (worker == player.getID())
    {
        m_waypointService.clearFor(player); // сел в своё — маркер больше не нужен
        return true;
    }
    player.sendClientMessage(ERROR_COLOUR, u("Это рабочее такси — за руль пускают только назначенного водителя"));
    return false;
}

// ------------------------------------------------------------------ очередь и выдача

void TaxiJobSystem::onSpotGranted(IPlayer &player)
{
    const int playerId = player.getID();
    const int spot = m_taxiJobService.spotOf(playerId);
    if (spot < 0 || spot >= TaxiJobService::SPOT_COUNT)
    {
        return;
    }

    IVehicle *taxi = m_vehicleService.create(TAXI_MODEL, SPAWN_SPOT[spot].position, SPAWN_SPOT[spot].angle,
                                             TAXI_COLOUR_1, TAXI_COLOUR_2, VehicleService::Owner::Work, -1);
    if (!taxi)
    {
        LogManager::log(Error, "TaxiJobSystem: vehicle pool full, taxi not spawned");
        // Точку не держим: вернуть работника в очередь честнее, чем оставить её
        // занятой без машины.
        m_taxiJobService.requeueTail(playerId);
        player.sendClientMessage(ERROR_COLOUR, u("Машину сейчас не подать. Вы возвращены в очередь"));
        return;
    }
    m_vehicleService.setInfiniteFuel(*taxi, true); // рабочий транспорт — бак не расходуется
    m_taxiJobService.setVehicle(playerId, taxi->getID());

    m_boardingSeconds[playerId] = 0;
    m_waypointService.showFor(player, *taxi); // красный маркер на поданное такси
    m_screenTimerService.show(player, BOARDING_SECONDS, BOARDING_TIMER_LABEL);
    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Такси подано и отмечено маркером. У вас {} секунд сесть за руль и "
                                           "отъехать — точка нужна другим",
                                           BOARDING_SECONDS)));
    m_screenNoticeService.show(player, "taxi ready - get in and drive off", BOARDING_POPUP_TIME,
                               NEUTRAL_POPUP_COLOUR);
}

void TaxiJobSystem::pumpQueue()
{
    bool promoted = false;
    for (;;)
    {
        const TaxiJobService::Promotion promotion = m_taxiJobService.promoteQueue(
            [this](int spot)
            {
                return spotClear(spot);
            });
        if (promotion.playerId < 0)
        {
            break;
        }
        IPlayer *next = m_core.getPlayers().get(promotion.playerId);
        if (!next)
        {
            m_taxiJobService.endShift(promotion.playerId);
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

void TaxiJobSystem::notifyQueueShift()
{
    const std::vector<int> queued = m_taxiJobService.queuedPlayers();
    for (int i = 0; i < static_cast<int>(queued.size()); ++i)
    {
        IPlayer *waiting = m_core.getPlayers().get(queued[i]);
        if (!waiting)
        {
            continue;
        }
        if (i == 0)
        {
            waiting->sendClientMessage(INFO_COLOUR, u("Очередь на такси продвинулась — вы следующий"));
        }
        else
        {
            waiting->sendClientMessage(INFO_COLOUR,
                                       u(fmt::format("Очередь на такси продвинулась, ваше место {}", i + 1)));
        }
    }
}

void TaxiJobSystem::failBoarding(IPlayer &player)
{
    const int playerId = player.getID();
    const int vid = m_taxiJobService.vehicleIdOf(playerId);

    m_waypointService.clearFor(player);
    m_screenTimerService.hide(player);
    m_taxiJobService.requeueTail(playerId);
    if (vid >= 0)
    {
        m_vehicleService.destroy(vid); // точка выезда не может стоять занятой
    }
    m_boardingSeconds[playerId] = 0;

    player.sendClientMessage(ERROR_COLOUR,
                             u("Вы не отъехали от таксопарка вовремя — машина снята. Вы возвращены в конец очереди"));
    m_screenNoticeService.show(player, "too slow - back to queue", FAIL_POPUP_TIME, ERROR_POPUP_COLOUR);
    notifyQueueShift();
}

// ------------------------------------------------------------------ поездка

void TaxiJobSystem::onTaxiCommand(IPlayer &passenger)
{
    const int passengerId = passenger.getID();
    const int driverId = m_taxiJobService.driverOfPassenger(passengerId);
    if (driverId < 0)
    {
        passenger.sendClientMessage(ERROR_COLOUR, u("Вы не в такси. Сядьте пассажиром к таксисту"));
        return;
    }

    const std::string body = "Выбрать место\nУказать на карте";
    m_dialogService.show(passenger, makeDialog(DialogStyle_LIST, "Куда едем?", body, "Выбрать", "Отмена"),
                         [this, passengerId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *rider = m_core.getPlayers().get(passengerId);
                             if (!rider || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             if (listItem == 0)
                             {
                                 showPlaceChoice(*rider);
                             }
                             else
                             {
                                 awaitMapClick(*rider);
                             }
                         });
}

void TaxiJobSystem::showPlaceChoice(IPlayer &passenger)
{
    const int passengerId = passenger.getID();
    const std::vector<PlaceCatalogService::Place> &places = m_placeCatalogService.places();

    std::string body;
    for (const PlaceCatalogService::Place &place : places)
    {
        body += place.name;
        body += "\n";
    }
    if (!body.empty())
    {
        body.pop_back(); // хвостовой '\n' дал бы пустую строку-фантом
    }

    m_dialogService.show(
        passenger, makeDialog(DialogStyle_LIST, "Куда едем?", body, "Выбрать", "Назад"),
        [this, passengerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *rider = m_core.getPlayers().get(passengerId);
            if (!rider || response != DialogResponse_Left)
            {
                return;
            }
            // Индекс от клиента — проверяем границы по ЖИВОМУ каталогу.
            const std::vector<PlaceCatalogService::Place> &current = m_placeCatalogService.places();
            if (listItem < 0 || listItem >= static_cast<int>(current.size()))
            {
                return;
            }
            // Водителя резолвим ЗАНОВО: пока висел диалог, пассажир мог выйти из такси.
            const int driverId = m_taxiJobService.driverOfPassenger(passengerId);
            if (driverId < 0)
            {
                rider->sendClientMessage(ERROR_COLOUR, u("Вы больше не в такси"));
                return;
            }
            applyDestination(driverId, passengerId, current[listItem].position, current[listItem].name);
        });
}

void TaxiJobSystem::awaitMapClick(IPlayer &passenger)
{
    const int passengerId = passenger.getID();
    if (!validPlayerId(passengerId))
    {
        return;
    }
    m_awaitingMapClick[passengerId] = true;
    passenger.sendClientMessage(INFO_COLOUR,
                                u("Откройте карту и кликните по месту, куда вас отвезти — водитель увидит точку"));
}

void TaxiJobSystem::onPlayerClickMap(IPlayer &player, Vector3 pos)
{
    const int passengerId = player.getID();
    if (!validPlayerId(passengerId) || !m_awaitingMapClick[passengerId])
    {
        return; // метку ставили не для нас
    }
    m_awaitingMapClick[passengerId] = false;

    const int driverId = m_taxiJobService.driverOfPassenger(passengerId);
    if (driverId < 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы больше не в такси"));
        return;
    }
    applyDestination(driverId, passengerId, pos, "метка на карте");
}

void TaxiJobSystem::applyDestination(int driverId, int passengerId, const Vector3 &destination,
                                     const std::string &name)
{
    IPlayer *driver = m_core.getPlayers().get(driverId);
    IPlayer *passenger = m_core.getPlayers().get(passengerId);
    if (!driver || !passenger)
    {
        return;
    }
    if (!m_taxiJobService.setDestination(driverId, destination, name))
    {
        return;
    }

    // Чекпоинт назначения — водителю. Слот его личного чекпоинта на смене принадлежит
    // этой работе (лок навигации взят при устройстве), поэтому конфликта с /gps нет.
    m_checkpointService.setForPlayer(*driver, destination, DESTINATION_CP_RADIUS,
                                     [this](IPlayer &p)
                                     {
                                         onDestinationReached(p);
                                     });

    const TaxiJobService::Ride *ride = m_taxiJobService.rideOf(driverId);
    const bool paid = ride != nullptr && ride->paid;

    passenger->sendClientMessage(INFO_COLOUR, u(fmt::format("Пункт назначения задан: {}", name)));
    driver->sendClientMessage(INFO_COLOUR,
                              u(fmt::format("Пассажир {}[{}] едет: {}. Чекпоинт поставлен", safeName(*passenger),
                                            passengerId, name)));
    if (!paid)
    {
        driver->sendClientMessage(INFO_COLOUR, u("Назначьте цену: /taxipay [сумма]"));
    }
}

void TaxiJobSystem::onTaxiPayCommand(IPlayer &driver, int fare)
{
    const int driverId = driver.getID();
    if (m_taxiJobService.phaseOf(driverId) != TaxiJobService::Phase::Working)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Назначать цену может только таксист на смене"));
        return;
    }
    const TaxiJobService::Ride *ride = m_taxiJobService.rideOf(driverId);
    if (!ride || ride->passengerId < 0)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("У вас нет пассажира"));
        return;
    }
    if (!ride->hasDestination)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Сначала пассажир должен указать адрес: /taxi"));
        return;
    }
    if (ride->paid)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Поездка уже оплачена"));
        return;
    }
    if (fare <= 0 || fare > MAX_FARE)
    {
        driver.sendClientMessage(ERROR_COLOUR, u(fmt::format("Сумма должна быть от 1 до {}", MAX_FARE)));
        return;
    }

    const int passengerId = ride->passengerId;
    IPlayer *passenger = m_core.getPlayers().get(passengerId);
    if (!passenger)
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Пассажир уже не в игре"));
        return;
    }
    // Проверка «у пассажира столько есть» — по СЕРВЕРНОМУ балансу, не по клиенту.
    if (!m_moneyService.canAfford(passengerId, static_cast<unsigned long long>(fare)))
    {
        driver.sendClientMessage(ERROR_COLOUR, u("У пассажира нет такой суммы — назовите цену ниже"));
        passenger->sendClientMessage(ERROR_COLOUR,
                                     u(fmt::format("Таксист хотел взять ${}, но у вас столько нет", fare)));
        return;
    }
    if (!m_taxiJobService.offerFare(driverId, fare))
    {
        return;
    }

    driver.sendClientMessage(INFO_COLOUR, u(fmt::format("Вы предложили пассажиру цену ${}. Ждём его ответа", fare)));
    m_dialogService.show(
        *passenger,
        makeDialog(DialogStyle_MSGBOX, "Оплата поездки",
                   fmt::format("Водитель {}[{}] просит за поездку ${}.\n\nМесто назначения: {}\n\nДеньги спишутся "
                               "сразу, но водитель получит их только когда довезёт вас. Не довёз — деньги "
                               "вернутся вам.",
                               safeName(driver), driverId, fare, ride->destinationName),
                   "Согласен", "Отказаться"),
        [this, driverId, passengerId](DialogResponse response, int, StringView)
        {
            IPlayer *rider = m_core.getPlayers().get(passengerId);
            if (!rider)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                rider->sendClientMessage(INFO_COLOUR, u("Вы отказались от поездки"));
                if (IPlayer *cabbie = m_core.getPlayers().get(driverId))
                {
                    cabbie->sendClientMessage(ERROR_COLOUR, u("Пассажир отказался от вашей цены"));
                }
                return;
            }
            onFareAccepted(driverId, passengerId);
        });
}

void TaxiJobSystem::onFareAccepted(int driverId, int passengerId)
{
    IPlayer *driver = m_core.getPlayers().get(driverId);
    IPlayer *passenger = m_core.getPlayers().get(passengerId);
    if (!driver || !passenger)
    {
        return;
    }

    // Ре-валидация на клике: пока висел диалог, пассажир мог выйти, цена — смениться,
    // деньги — уйти на другое.
    const TaxiJobService::Ride *ride = m_taxiJobService.rideOf(driverId);
    if (!ride || ride->passengerId != passengerId || ride->paid || ride->fare <= 0)
    {
        passenger->sendClientMessage(ERROR_COLOUR, u("Договор уже неактуален"));
        return;
    }
    const std::int64_t fare = ride->fare;
    if (!m_moneyService.take(*passenger, static_cast<unsigned long long>(fare)))
    {
        passenger->sendClientMessage(ERROR_COLOUR, u("У вас не хватает денег на поездку"));
        driver->sendClientMessage(ERROR_COLOUR, u("У пассажира не хватило денег"));
        return;
    }
    if (!m_taxiJobService.confirmFare(driverId))
    {
        // Списали, но поездка успела закрыться — деньги немедленно назад.
        m_moneyService.giveMoney(*passenger, static_cast<unsigned long long>(fare));
        return;
    }

    passenger->sendClientMessage(INFO_COLOUR,
                                 u(fmt::format("Вы оплатили ${}. Водитель получит их, когда довезёт вас", fare)));
    driver->sendClientMessage(INFO_COLOUR,
                              u(fmt::format("Пассажир согласился на ${}. Деньги получите по прибытии", fare)));
}

void TaxiJobSystem::onDestinationReached(IPlayer &driver)
{
    const int driverId = driver.getID();
    const TaxiJobService::Ride *ride = m_taxiJobService.rideOf(driverId);
    if (!ride || ride->passengerId < 0 || !ride->hasDestination)
    {
        return; // чекпоинт не наш / поездка уже закрыта
    }
    // Довезти — значит довезти ПАССАЖИРА: приехать одному и получить деньги нельзя.
    if (!passengerAboard(driverId))
    {
        driver.sendClientMessage(ERROR_COLOUR, u("Пассажира нет в машине — поездка не засчитана"));
        return;
    }
    closeRide(driverId, /*delivered=*/true, {});
}

bool TaxiJobSystem::passengerAboard(int driverId) const
{
    const TaxiJobService::Ride *ride = m_taxiJobService.rideOf(driverId);
    if (!ride || ride->passengerId < 0)
    {
        return false;
    }
    const int vid = m_taxiJobService.vehicleIdOf(driverId);
    if (vid < 0)
    {
        return false;
    }
    // Серверная привязка «игрок -> машина», не заявление клиента.
    IVehicle *passengerVehicle = m_vehicleService.getVehicle(ride->passengerId);
    return passengerVehicle != nullptr && passengerVehicle->getID() == vid;
}

void TaxiJobSystem::closeRide(int driverId, bool delivered, const std::string &reasonForPassenger)
{
    const TaxiJobService::Ride ride = m_taxiJobService.endRide(driverId);
    if (ride.passengerId < 0)
    {
        return; // поездки не было
    }

    IPlayer *driver = m_core.getPlayers().get(driverId);
    IPlayer *passenger = m_core.getPlayers().get(ride.passengerId);

    if (driver)
    {
        m_checkpointService.clearForPlayer(*driver); // цель поездки снята в любом случае
    }

    if (!ride.paid)
    {
        // Денег не вносили — возвращать нечего, просто закрываем.
        if (passenger && !delivered && !reasonForPassenger.empty())
        {
            passenger->sendClientMessage(ERROR_COLOUR, u(reasonForPassenger));
        }
        return;
    }

    if (delivered)
    {
        if (driver)
        {
            m_moneyService.giveMoney(*driver, static_cast<unsigned long long>(ride.fare));
            driver->sendClientMessage(INFO_COLOUR, u(fmt::format("Пассажир доставлен. Вы получили ${}", ride.fare)));
            m_screenNoticeService.show(*driver, fmt::format("fare +${}", ride.fare), CREDIT_POPUP_TIME,
                                       CREDIT_POPUP_COLOUR);
        }
        if (passenger)
        {
            passenger->sendClientMessage(INFO_COLOUR, u("Вы на месте. Спасибо за поездку!"));
        }
        return;
    }

    // Не довезли — депозит ВСЕГДА возвращается пассажиру. Если пассажира уже нет в
    // игре, деньги теряются (наличные сессионные), поэтому возврат идёт как можно
    // раньше: сессия пассажира ещё активна на subscribeEnd.
    if (passenger)
    {
        m_moneyService.giveMoney(*passenger, static_cast<unsigned long long>(ride.fare));
        passenger->sendClientMessage(
            ERROR_COLOUR, u(reasonForPassenger.empty()
                                ? fmt::format("Поездка не состоялась. Вам возвращено ${}", ride.fare)
                                : fmt::format("{}. Вам возвращено ${}", reasonForPassenger, ride.fare)));
    }
    if (driver)
    {
        driver->sendClientMessage(ERROR_COLOUR, u("Поездка не состоялась — деньги вернулись пассажиру"));
    }
}

// ------------------------------------------------------------------ таймер таксопарка

void TaxiJobSystem::onDepotTick()
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
        const TaxiJobService::Phase phase = m_taxiJobService.phaseOf(playerId);
        if (phase == TaxiJobService::Phase::Boarding || phase == TaxiJobService::Phase::Working)
        {
            tickWorker(*player);
        }
    }
    pumpQueue(); // освободившиеся точки — следующим из очереди
}

void TaxiJobSystem::tickWorker(IPlayer &player)
{
    const int playerId = player.getID();

    // Смена держит ЛИЧНОЕ такси: пропало (уничтожено/деспавн) -> увольнение штатно.
    const int vid = m_taxiJobService.vehicleIdOf(playerId);
    if (vid >= 0 && !m_vehicleService.get(vid))
    {
        dismiss(player, "Такси пропало — смена окончена", ERROR_COLOUR);
        return;
    }

    if (m_taxiJobService.phaseOf(playerId) == TaxiJobService::Phase::Boarding)
    {
        const int spot = m_taxiJobService.spotOf(playerId);
        // Окно закрывает ОТЪЕЗД, а не посадка: точка нужна следующему из очереди.
        if (!taxiOnSpot(vid, spot))
        {
            m_taxiJobService.completeBoarding(playerId);
            m_boardingSeconds[playerId] = 0;
            m_awaySeconds[playerId] = 0; // окно возврата начинается с чистого листа
            m_waypointService.clearFor(player);
            m_screenTimerService.hide(player);
            player.sendClientMessage(INFO_COLOUR,
                                     u("Вы на смене. Подбирайте пассажиров — они укажут адрес сами"));
            m_screenNoticeService.show(player, "on duty - pick up passengers", BOARDING_POPUP_TIME,
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
        return;
    }

    // Working: водитель обязан быть за рулём своего такси. Вышел — окно возврата.
    if (!drivingOwnTaxi(playerId))
    {
        const int away = ++m_awaySeconds[playerId];
        if (away >= RETURN_SECONDS)
        {
            dismiss(player, "Вы бросили такси — вы уволены", ERROR_COLOUR);
            return;
        }
        m_screenTimerService.show(player, RETURN_SECONDS - away + 1, RETURN_TIMER_LABEL);
        return; // пока водителя нет за рулём, поездку не двигаем
    }
    if (m_awaySeconds[playerId] != 0)
    {
        m_awaySeconds[playerId] = 0;
        m_screenTimerService.hide(player);
    }

    // Следим за пассажиром. Он «в поездке», пока физически в машине —
    // вышел (сам, телепортом, смертью) значит поездка кончилась, депозит назад.
    const TaxiJobService::Ride *ride = m_taxiJobService.rideOf(playerId);
    if (ride && ride->passengerId >= 0)
    {
        if (!passengerAboard(playerId))
        {
            closeRide(playerId, /*delivered=*/false, "Вы вышли из такси, не доехав");
        }
        return;
    }

    // Пассажира нет — не сел ли кто? Определяем по СЕРВЕРНОЙ привязке к машине.
    if (vid < 0)
    {
        return;
    }
    for (IPlayer *candidate : m_core.getPlayers().entries())
    {
        if (!candidate || candidate->getID() == playerId)
        {
            continue;
        }
        IVehicle *vehicle = m_vehicleService.getVehicle(candidate->getID());
        if (!vehicle || vehicle->getID() != vid || m_vehicleService.getSeat(candidate->getID()) <= 0)
        {
            continue; // не в этом такси либо за рулём
        }
        if (!m_taxiJobService.setPassenger(playerId, candidate->getID()))
        {
            continue;
        }
        candidate->sendClientMessage(INFO_COLOUR, u("Чтобы указать место назначения введите /taxi"));
        player.sendClientMessage(INFO_COLOUR,
                                 u(fmt::format("К вам сел пассажир {}[{}]", safeName(*candidate),
                                               candidate->getID())));
        break; // поездка одна — остальные попутчики её не меняют
    }
}

// ------------------------------------------------------------------ увольнение

void TaxiJobSystem::dismiss(IPlayer &player, const std::string &reason, const Colour &colour)
{
    if (!m_taxiJobService.isWorking(player.getID()))
    {
        return; // защитно
    }
    teardownShift(player);
    player.sendClientMessage(colour, u(reason));
}

void TaxiJobSystem::teardownShift(IPlayer &player)
{
    const int playerId = player.getID();

    // Поездку закрываем ДО endShift: депозит обязан вернуться пассажиру, а после
    // сброса состояния о нём уже никто не узнает.
    closeRide(playerId, /*delivered=*/false, "Ваш таксист не довёз вас");

    m_waypointService.clearFor(player);
    m_checkpointService.clearForPlayer(player);
    m_screenTimerService.hide(player);

    const int vid = m_taxiJobService.vehicleIdOf(playerId);
    m_taxiJobService.endShift(playerId);
    if (vid >= 0)
    {
        m_vehicleService.destroy(vid); // личное такси не остаётся хламом
    }
    if (validPlayerId(playerId))
    {
        m_boardingSeconds[playerId] = 0;
        m_awaySeconds[playerId] = 0;
        m_awaitingMapClick[playerId] = false;
    }
    m_navLockService.release(playerId);
    pumpQueue();
}

// ------------------------------------------------------------------ лайфцикл сессии

void TaxiJobSystem::onSessionEnd(IPlayer &player)
{
    const int playerId = player.getID();

    // Уходит ПАССАЖИР: его поездку закрывает водитель, но депозит вернуть надо сейчас,
    // пока игрок ещё в игре и наличные ему можно отдать.
    const int driverId = m_taxiJobService.driverOfPassenger(playerId);
    if (driverId >= 0)
    {
        closeRide(driverId, /*delivered=*/false, "Вы вышли из игры, не доехав");
    }

    if (m_taxiJobService.isWorking(playerId))
    {
        const bool wasQueued = m_taxiJobService.phaseOf(playerId) == TaxiJobService::Phase::Queued;
        teardownShift(player); // без сообщения — игрок уходит
        if (wasQueued)
        {
            notifyQueueShift();
        }
    }
    if (validPlayerId(playerId))
    {
        m_awaitingMapClick[playerId] = false;
    }
}

void TaxiJobSystem::onPlayerDeath(IPlayer &player)
{
    const int playerId = player.getID();

    // Погиб ПАССАЖИР — поездка кончилась, деньги назад.
    const int driverId = m_taxiJobService.driverOfPassenger(playerId);
    if (driverId >= 0)
    {
        closeRide(driverId, /*delivered=*/false, "Вы погибли, не доехав");
    }

    if (!m_taxiJobService.isWorking(playerId))
    {
        return;
    }
    dismiss(player, "Вы погибли — смена таксиста завершена", ERROR_COLOUR);
}

// ------------------------------------------------------------------ helpers

bool TaxiJobSystem::drivingOwnTaxi(int playerId) const
{
    const int vid = m_taxiJobService.vehicleIdOf(playerId);
    return vid >= 0 && m_vehicleService.getDriver(vid) == playerId;
}

bool TaxiJobSystem::spotClear(int spot) const
{
    if (spot < 0 || spot >= TaxiJobService::SPOT_COUNT)
    {
        return false; // вне диапазона -> считаем занятой (не спавним)
    }
    const Vector3 &centre = SPAWN_SPOT[spot].position;
    const float angle = SPAWN_SPOT[spot].angle;
    return !m_vehicleService.anyVehicleNear(centre, SPOT_OCCUPIED_RADIUS) &&
           !m_vehicleService.anyVehicleNear(Geometry::forwardOf(centre, angle, SPOT_PROBE_OFFSET),
                                            SPOT_OCCUPIED_RADIUS) &&
           !m_vehicleService.anyVehicleNear(Geometry::backOf(centre, angle, SPOT_PROBE_OFFSET),
                                            SPOT_OCCUPIED_RADIUS);
}

bool TaxiJobSystem::taxiOnSpot(int vehicleId, int spot) const
{
    if (vehicleId < 0 || spot < 0 || spot >= TaxiJobService::SPOT_COUNT)
    {
        return false;
    }
    IVehicle *taxi = m_vehicleService.get(vehicleId);
    if (!taxi)
    {
        return false;
    }
    return distanceSq2D(taxi->getPosition(), SPAWN_SPOT[spot].position) <= SPOT_RADIUS * SPOT_RADIUS;
}
