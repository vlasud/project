#include "Services/Core/VehicleService/VehicleService.h"

#include "glm/geometric.hpp"
#include <chrono>
#include <cmath>
#include <fmt/format.h>

namespace
{
// Допуск на дрожание float HP между клиентом и сервером.
constexpr float HEALTH_EPS = 5.0f;


// Грейс после серверного setHealth/repair: клиент применяет RPC.
constexpr std::chrono::milliseconds SYNC_GRACE{1500};

// Rate limit повторных нарушений по одной машине.
constexpr std::chrono::milliseconds FLAG_COOLDOWN{2000};

// Unoccupied sync легально шлёт клиент, у которого машина застримлена (~300 м);
// дальше — физически невозможно.
constexpr float UNOCCUPIED_REPORTER_MAX = 350.0f;
// Пустая машина за один апдейт не прыгает дальше этого (катится с горки ~ метры).
constexpr float UNOCCUPIED_JUMP_MAX = 15.0f;
// И не движется быстрее (свободное качение под горку).
constexpr float UNOCCUPIED_SPEED_MAX = 60.0f;

// Прицеп должен быть рядом с тягачом репортера.
constexpr float TRAILER_MAX_DIST = 50.0f;

TimePoint now()
{
    return std::chrono::steady_clock::now();
}

bool rateLimited(TimePoint &lastFlag, TimePoint timeNow)
{
    if (timeNow - lastFlag < FLAG_COOLDOWN)
        return true;
    lastFlag = timeNow;
    return false;
}
} // namespace

void VehicleService::bind(IVehiclesComponent *vehicles, PlayerLocationService &location,
                          VehicleEventHandler &vehicleEvents, PoolEventHandler<IVehicle> &poolEvents)
{
    m_vehicles = vehicles;
    m_location = &location;
    if (m_vehicles)
    {
        m_vehicles->getEventDispatcher().addEventHandler(&vehicleEvents);
        m_vehicles->getPoolEventDispatcher().addEventHandler(&poolEvents);
    }
}

IVehicle *VehicleService::get(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicles)
        return nullptr;
    return m_vehicles->get(vehicleId);
}

void VehicleService::destroy(int vehicleId)
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicles)
        return;
    m_vehicles->release(vehicleId);
}

void VehicleService::subscribeCreated(VehicleObserver observer)
{
    m_createdObservers.push_back(std::move(observer));
}

void VehicleService::subscribeDestroyed(VehicleObserver observer)
{
    m_destroyedObservers.push_back(std::move(observer));
}

IVehicle *VehicleService::create(int model, Vector3 position, float angle, int colour1, int colour2, Owner owner,
                                 int ownerId)
{
    if (!m_vehicles)
        return nullptr;

    // Валидный диапазон моделей машин SA (400..611): create — единая блессед-точка
    // создания, мусорную/невалидную модель в SDK не пропускаем.
    if (model < 400 || model > 611)
        return nullptr;

    VehicleSpawnData data;
    data.respawnDelay = Seconds(-1); // без авто-респауна; политику решает бизнес
    data.modelID = model;
    data.position = position;
    data.zRotation = angle;
    data.colour1 = colour1;
    data.colour2 = colour2;
    data.siren = false;
    data.interior = 0;

    IVehicle *vehicle = m_vehicles->create(data);
    if (!vehicle)
        return nullptr; // пул машин полон

    // Создание уже прогнало стейт через onVehicleCreated (exists/HP/полный бак,
    // owner=None). Проставляем владельца поверх готового стейта.
    setOwner(*vehicle, owner, ownerId);
    return vehicle;
}

IVehicle *VehicleService::getVehicle(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS || !m_vehicles)
        return nullptr;
    const int vehicleId = m_occupants[playerId].vehicleId;
    if (vehicleId < 0)
        return nullptr;
    return m_vehicles->get(vehicleId);
}

int VehicleService::getSeat(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return -1;
    return m_occupants[playerId].seat;
}

int VehicleService::getDriver(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return -1;
    return m_vehicleState[vehicleId].driverId;
}

float VehicleService::getHealth(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return 0.0f;
    return m_vehicleState[vehicleId].health;
}

Vector3 VehicleService::getVelocity(int vehicleId) const
{
    // Клиентская велосити (SA-юниты) прямо из пула — кэшированного стейта для неё
    // нет (косметика, в логике не участвует). getVelocity() в SDK не const, но
    // m_vehicles->get отдаёт неконстантный IVehicle*, как и публичный get().
    IVehicle *vehicle = get(vehicleId);
    if (!vehicle)
        return Vector3(0.0f, 0.0f, 0.0f);
    return vehicle->getVelocity();
}

void VehicleService::setHealth(IVehicle &vehicle, float health)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st.health = health < 0.0f ? 0.0f : health;
    st.lastChange = now();
    vehicle.setHealth(st.health);
    stallIfCritical(vehicle, st, now());
}

void VehicleService::repair(IVehicle &vehicle)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st.health = MAX_HEALTH;
    st.lastChange = now();
    vehicle.repair(); // полный ремонт: HP + визуальные повреждения
    clearStall(vehicle, st);
}

void VehicleService::applyDamage(IVehicle &vehicle, float amount)
{
    if (amount <= 0.0f)
        return;
    VehicleState &st = m_vehicleState[vehicle.getID()];
    if (!st.exists)
        return;
    st.health -= amount;
    if (st.health < 0.0f)
        st.health = 0.0f;
    st.lastChange = now(); // грейс: честный водитель применит setHealth и сойдётся
    vehicle.setHealth(st.health);
    stallIfCritical(vehicle, st, now());
}

void VehicleService::setEngine(IVehicle &vehicle, bool on)
{
    const VehicleState &st = m_vehicleState[vehicle.getID()];
    if (on && (st.stalled || st.outOfFuel))
        return; // не завести: добитую — repair(), пустую — refuel()
    VehicleParams params = vehicle.getParams();
    params.engine = on ? 1 : 0;
    vehicle.setParams(params);
}

void VehicleService::setLights(IVehicle &vehicle, bool on)
{
    // Фары не зависят от «заглохла»: их можно жечь и на добитой машине.
    VehicleParams params = vehicle.getParams();
    params.lights = on ? 1 : 0;
    vehicle.setParams(params);
}

void VehicleService::stallIfCritical(IVehicle &vehicle, VehicleState &st, TimePoint timeNow)
{
    if (st.health > STALL_HEALTH)
        return;
    // Машины не взрываются ВООБЩЕ: ниже ~250 клиент поджигает и затем
    // взрывает. Держим HP над порогом пожара (восстановление HP тушит уже
    // занявшийся огонь) и глушим двигатель — ездить на добитой нельзя.
    st.health = STALL_HEALTH;
    st.lastChange = timeNow;
    vehicle.setHealth(STALL_HEALTH);
    if (!st.stalled)
    {
        st.stalled = true;
        VehicleParams params = vehicle.getParams();
        params.engine = 0;
        vehicle.setParams(params);
    }
}

void VehicleService::clearStall(IVehicle &vehicle, VehicleState &st)
{
    if (!st.stalled)
        return;
    st.stalled = false;
    // Возвращаем двигателю клиентский авто-режим (-1): заводится при посадке, как
    // обычная машина. НО если ещё и пустой бак (outOfFuel) — оставляем выключенным
    // (0): repair чинит HP, не топливо; заведётся только после refuel. Иначе
    // отремонтированная пустая машина ездила бы с пустым баком.
    VehicleParams params = vehicle.getParams();
    params.engine = st.outOfFuel ? 0 : -1;
    vehicle.setParams(params);
}

bool VehicleService::isStalled(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE)
        return false;
    return m_vehicleState[vehicleId].stalled;
}

VehicleService::Owner VehicleService::getOwner(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return Owner::None;
    return m_vehicleState[vehicleId].owner;
}

int VehicleService::getOwnerId(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return -1;
    return m_vehicleState[vehicleId].ownerId;
}

void VehicleService::setOwner(IVehicle &vehicle, Owner owner, int ownerId)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    if (!st.exists)
        return;
    st.owner = owner;
    st.ownerId = owner == Owner::None ? -1 : ownerId;
}

float VehicleService::getFuel(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return 0.0f;
    return m_vehicleState[vehicleId].fuel;
}

bool VehicleService::isOutOfFuel(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE)
        return false;
    return m_vehicleState[vehicleId].outOfFuel;
}

void VehicleService::refuel(IVehicle &vehicle, float amount)
{
    if (!std::isfinite(amount) || amount <= 0.0f)
        return; // NaN/Inf (NaN<=0 == false) не должен отравить fuel
    VehicleState &st = m_vehicleState[vehicle.getID()];
    if (!st.exists)
        return;
    st.fuel += amount;
    if (st.fuel > FUEL_CAPACITY)
        st.fuel = FUEL_CAPACITY;
    // Снимаем «пустой бак»: двигатель снова можно завести — но НЕ заводим сами,
    // игрок заведёт сам через Fire (setEngine теперь разрешён). «Заглохла» (HP) —
    // это другая причина и refuel её не снимает.
    if (st.fuel > 0.0f)
        st.outOfFuel = false;
}

void VehicleService::setFuel(IVehicle &vehicle, float amount)
{
    if (!std::isfinite(amount))
        return; // NaN/Inf не осядет в fuel (NaN сравнения ниже неопределены)
    VehicleState &st = m_vehicleState[vehicle.getID()];
    if (!st.exists)
        return;
    st.fuel = amount < 0.0f ? 0.0f : (amount > FUEL_CAPACITY ? FUEL_CAPACITY : amount);
    if (st.fuel > 0.0f)
        st.outOfFuel = false;
}

void VehicleService::drainFuel(float seconds)
{
    if (seconds <= 0.0f || !m_vehicles)
        return;
    const float drain = FUEL_DRAIN_PER_SEC * seconds;

    // Проход по пулу: дешёвые bool-проверки отсекают пустые слоты и пустые баки;
    // getParams читаем только у существующих машин с топливом. Зовётся по таймеру.
    // Расход зависит ТОЛЬКО от факта работающего двигателя (наличие водителя ни при
    // чём): запущенный двигатель жжёт топливо.
    for (int vehicleId = 0; vehicleId < VEHICLE_POOL_SIZE; ++vehicleId)
    {
        VehicleState &st = m_vehicleState[vehicleId];
        if (!st.exists || st.fuel <= 0.0f)
            continue; // нет машины / пустой бак — пропуск

        IVehicle *vehicle = m_vehicles->get(vehicleId);
        if (!vehicle)
            continue; // машины уже нет в пуле — стейт подчистит destroyed-событие

        if (vehicle->getParams().engine == 0)
            continue; // двигатель заглушён (engine == 0) — топливо не расходуется

        st.fuel -= drain;
        if (st.fuel > 0.0f)
            continue;

        // Бак опустел: глушим двигатель и метим outOfFuel (снимет только refuel).
        st.fuel = 0.0f;
        st.outOfFuel = true;
        VehicleParams params = vehicle->getParams();
        params.engine = 0;
        vehicle->setParams(params);
    }
}

void VehicleService::setLocked(IVehicle &vehicle, bool locked)
{
    VehicleParams params = vehicle.getParams();
    params.doors = locked ? 1 : 0;
    vehicle.setParams(params);
}

void VehicleService::addComponent(IVehicle &vehicle, int component)
{
    vehicle.addComponent(component);
}

void VehicleService::removeComponent(IVehicle &vehicle, int component)
{
    vehicle.removeComponent(component);
}

void VehicleService::setPaintJob(IVehicle &vehicle, int paintjob)
{
    vehicle.setPaintJob(paintjob);
}

void VehicleService::sanctionRepair(int vehicleId, TimePoint timeNow)
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return;
    // Клиент мог легально починить машину (мод-шоп, Pay'n'Spray) — принимаем
    // полное HP и даём грейс, чтобы repair-hack детектор не дал ложняк.
    VehicleState &st = m_vehicleState[vehicleId];
    st.health = MAX_HEALTH;
    st.lastChange = timeNow;
    if (IVehicle *vehicle = m_vehicles ? m_vehicles->get(vehicleId) : nullptr)
        clearStall(*vehicle, st); // починенная снова заводится
}

bool VehicleService::isDriverOf(int playerId, const IVehicle &vehicle) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    const Occupant &occupant = m_occupants[playerId];
    return occupant.seat == 0 && occupant.vehicleId == vehicle.getID();
}

void VehicleService::bindOccupant(IPlayer &player, PlayerState newState)
{
    const int playerId = player.getID();
    Occupant &occupant = m_occupants[playerId];

    // Снимаем старую привязку «машина -> водитель», если она была наша.
    if (occupant.seat == 0 && occupant.vehicleId >= 0 && m_vehicleState[occupant.vehicleId].driverId == playerId)
    {
        m_vehicleState[occupant.vehicleId].driverId = -1;
    }

    if (newState == PlayerState_Driver || newState == PlayerState_Passenger)
    {
        // Граница сервиса: queryExtension здесь, потребители ходят через getVehicle().
        IPlayerVehicleData *data = queryExtension<IPlayerVehicleData>(player);
        IVehicle *vehicle = data ? data->getVehicle() : nullptr;
        occupant.vehicleId = vehicle ? vehicle->getID() : -1;
        occupant.seat = data ? data->getSeat() : -1;

        if (occupant.seat == 0 && occupant.vehicleId >= 0)
        {
            m_vehicleState[occupant.vehicleId].driverId = playerId;
        }
        return;
    }

    occupant = {};
}

VehicleService::Outcome VehicleService::verifyHealth(IPlayer &player, TimePoint timeNow)
{
    Outcome outcome;

    const Occupant &occupant = m_occupants[player.getID()];
    if (occupant.seat != 0 || occupant.vehicleId < 0) // HP диктует только водитель
        return outcome;

    VehicleState &st = m_vehicleState[occupant.vehicleId];
    if (!st.exists || !m_vehicles)
        return outcome;
    IVehicle *vehicle = m_vehicles->get(occupant.vehicleId);
    if (!vehicle)
        return outcome;

    const float reported = vehicle->getHealth(); // заявление клиента водителя

    // Мусорный float (NaN отсеялся бы сравнением ниже, но -inf прошёл бы в
    // принятие и осел в серверном HP) — откат на серверную правду.
    if (!std::isfinite(reported) || reported < 0.0f)
    {
        vehicle->setHealth(st.health);
        st.lastChange = timeNow;
        return outcome;
    }

    if (reported <= st.health + HEALTH_EPS)
    {
        // Снижение (урон) или совпадение — принимаем как новую правду.
        st.health = reported;
        stallIfCritical(*vehicle, st, timeNow); // у порога пожара — кламп + глушим
        return outcome;
    }

    if (timeNow - st.lastChange < SYNC_GRACE)
        return outcome; // клиент применяет недавний серверный setHealth/repair

    // Рост HP без серверной санкции — vehicle repair hack. Откатываем.
    vehicle->setHealth(st.health);
    st.lastChange = timeNow;
    if (!rateLimited(st.lastFlag, timeNow))
    {
        outcome.vehicleHack = true;
        outcome.detail =
            fmt::format("vehicle {} repair hack: {:.0f} -> {:.0f}", occupant.vehicleId, st.health, reported);
    }
    return outcome;
}

VehicleService::Outcome VehicleService::validateUnoccupied(IVehicle &vehicle, IPlayer &reporter,
                                                           const UnoccupiedVehicleUpdate &update, TimePoint timeNow)
{
    Outcome outcome;
    VehicleState &st = m_vehicleState[vehicle.getID()];

    // Машину двигает сервер (редактор): её телепорты и дальний репортер легальны.
    if (st.editBypass)
    {
        return outcome;
    }

    const auto reject = [&](std::string detail)
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = std::move(detail);
        }
        outcome.vehicleHack = true;
        return outcome;
    };

    if (m_location)
    {
        const float reporterDist = glm::distance(m_location->getPosition(reporter.getID()), vehicle.getPosition());
        if (reporterDist > UNOCCUPIED_REPORTER_MAX)
        {
            return reject(fmt::format("unoccupied sync for vehicle {} from {:.0f}m", vehicle.getID(), reporterDist));
        }
    }

    const float jump = glm::distance(vehicle.getPosition(), update.position);
    if (jump > UNOCCUPIED_JUMP_MAX)
    {
        return reject(fmt::format("unoccupied vehicle {} jump {:.0f}m", vehicle.getID(), jump));
    }

    const float speed = glm::length(update.velocity);
    if (speed > UNOCCUPIED_SPEED_MAX)
    {
        return reject(fmt::format("unoccupied vehicle {} speed {:.0f} m/s", vehicle.getID(), speed));
    }

    return outcome; // легально
}

VehicleService::Outcome VehicleService::validateTrailer(IPlayer &reporter, IVehicle &trailer, TimePoint timeNow)
{
    Outcome outcome;
    if (!m_location)
        return outcome;

    VehicleState &st = m_vehicleState[trailer.getID()];
    const float dist = glm::distance(m_location->getPosition(reporter.getID()), trailer.getPosition());
    if (dist > TRAILER_MAX_DIST)
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = fmt::format("trailer {} sync from {:.0f}m", trailer.getID(), dist);
        }
        outcome.vehicleHack = true;
    }
    return outcome;
}

VehicleService::Outcome VehicleService::validateMod(IPlayer &player, IVehicle &vehicle, int component,
                                                    TimePoint timeNow)
{
    Outcome outcome;
    VehicleState &st = m_vehicleState[vehicle.getID()];

    const auto reject = [&](std::string detail)
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = std::move(detail);
        }
        outcome.vehicleHack = true;
        return outcome;
    };

    if (!isDriverOf(player.getID(), vehicle))
    {
        return reject(fmt::format("mod {} for vehicle {} not as driver", component, vehicle.getID()));
    }

    if (component < 1000 || component > 1193) // валидные id компонентов SA-MP
    {
        return reject(fmt::format("invalid mod component {}", component));
    }

    // Легальный тюнинг возможен только внутри мод-шопа. Чит-меню ставит моды
    // (нитро, гидравлику) где угодно — это и отсекаем.
    IPlayerVehicleData *data = queryExtension<IPlayerVehicleData>(player);
    if (!data || !data->isInModShop())
    {
        return reject(fmt::format("mod {} outside mod shop", component));
    }

    // Принято: покупка детали чинит машину на клиенте — санкция ремонта.
    sanctionRepair(vehicle.getID(), timeNow);
    return outcome;
}

VehicleService::Outcome VehicleService::validateRespray(IPlayer &player, IVehicle &vehicle, TimePoint timeNow)
{
    Outcome outcome;
    VehicleState &st = m_vehicleState[vehicle.getID()];

    if (!isDriverOf(player.getID(), vehicle))
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = fmt::format("respray of vehicle {} not as driver", vehicle.getID());
        }
        outcome.vehicleHack = true;
        return outcome;
    }

    // Pay'n'Spray и мод-шоп при перекраске чинят машину на клиенте.
    sanctionRepair(vehicle.getID(), timeNow);
    return outcome;
}

void VehicleService::onModShop(IPlayer &player, TimePoint timeNow)
{
    const Occupant &occupant = m_occupants[player.getID()];
    if (occupant.seat == 0)
    {
        sanctionRepair(occupant.vehicleId, timeNow);
    }
}

void VehicleService::setEditBypass(int vehicleId, bool enable)
{
    if (vehicleId < 0 || vehicleId >= static_cast<int>(m_vehicleState.size()))
    {
        return;
    }
    m_vehicleState[vehicleId].editBypass = enable;
}

void VehicleService::onVehicleCreated(IVehicle &vehicle)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st = {}; // сброс: owner=None, бак полон (FUEL_CAPACITY), outOfFuel=false
    st.exists = true;
    st.health = vehicle.getHealth();
    st.lastChange = now();
    // Стейт готов — оповещаем наблюдателей (например, GridSystem добавляет в сетку).
    for (auto &obs : m_createdObservers)
        obs(vehicle);
}

void VehicleService::onVehicleDestroyed(IVehicle &vehicle)
{
    // Наблюдатели вызываются пока машина ещё валидна (до сброса стейта).
    for (auto &obs : m_destroyedObservers)
        obs(vehicle);
    m_vehicleState[vehicle.getID()] = {};
}

void VehicleService::onVehicleRespawn(IVehicle &vehicle)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st.exists = true;
    st.health = MAX_HEALTH;
    st.fuel = FUEL_CAPACITY; // респаун — бак снова полный (как HP)
    st.outOfFuel = false;
    st.lastChange = now();
    clearStall(vehicle, st); // респаун — машина снова целая и заводится
    // owner сохраняется: ту же физическую машину переспавнили — владелец тот же.
}

void VehicleService::onVehicleDeath(IVehicle &vehicle)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st.health = 0.0f;
    st.lastChange = now();
}

void VehicleService::resetPlayer(int playerId)
{
    const Occupant &occupant = m_occupants[playerId];
    if (occupant.seat == 0 && occupant.vehicleId >= 0 && m_vehicleState[occupant.vehicleId].driverId == playerId)
    {
        m_vehicleState[occupant.vehicleId].driverId = -1;
    }
    m_occupants[playerId] = {};
}
