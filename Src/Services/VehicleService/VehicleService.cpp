#include "VehicleService.h"

#include "glm/geometric.hpp"
#include <chrono>
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

void VehicleService::bind(IVehiclesComponent *vehicles, PlayerLocationService &location)
{
    m_vehicles = vehicles;
    m_location = &location;
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

void VehicleService::setHealth(IVehicle &vehicle, float health)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st.health = health < 0.0f ? 0.0f : health;
    st.lastChange = now();
    vehicle.setHealth(st.health);
}

void VehicleService::repair(IVehicle &vehicle)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st.health = 1000.0f;
    st.lastChange = now();
    vehicle.repair(); // полный ремонт: HP + визуальные повреждения
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
}

void VehicleService::setEngine(IVehicle &vehicle, bool on)
{
    VehicleParams params = vehicle.getParams();
    params.engine = on ? 1 : 0;
    vehicle.setParams(params);
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
    st.health = 1000.0f;
    st.lastChange = timeNow;
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

    if (reported <= st.health + HEALTH_EPS)
    {
        // Снижение (урон) или совпадение — принимаем как новую правду.
        st.health = reported;
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

void VehicleService::onVehicleCreated(IVehicle &vehicle)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st = {};
    st.exists = true;
    st.health = vehicle.getHealth();
    st.lastChange = now();
}

void VehicleService::onVehicleDestroyed(IVehicle &vehicle)
{
    m_vehicleState[vehicle.getID()] = {};
}

void VehicleService::onVehicleRespawn(IVehicle &vehicle)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st.exists = true;
    st.health = 1000.0f;
    st.lastChange = now();
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
