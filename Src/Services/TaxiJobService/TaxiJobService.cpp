#include "Services/TaxiJobService/TaxiJobService.h"

#include <algorithm>
#include <utility>

namespace
{
bool validId(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}
} // namespace

TaxiJobService::TaxiJobService()
{
    m_spotHolder.fill(-1); // 0 — валидный playerId, «свободно» это -1
}

TaxiJobService::Phase TaxiJobService::phaseOf(int playerId) const
{
    if (!validId(playerId))
    {
        return Phase::NotWorking;
    }
    return m_state[playerId].phase;
}

bool TaxiJobService::isWorking(int playerId) const
{
    return phaseOf(playerId) != Phase::NotWorking;
}

int TaxiJobService::vehicleIdOf(int playerId) const
{
    if (!validId(playerId))
    {
        return -1;
    }
    return m_state[playerId].vehicleId;
}

int TaxiJobService::queuePositionOf(int playerId) const
{
    int pos = 1;
    for (const int queued : m_queue)
    {
        if (queued == playerId)
        {
            return pos;
        }
        ++pos;
    }
    return 0;
}

std::vector<int> TaxiJobService::queuedPlayers() const
{
    return std::vector<int>(m_queue.begin(), m_queue.end());
}

int TaxiJobService::spotOf(int playerId) const
{
    if (!validId(playerId))
    {
        return -1;
    }
    return m_state[playerId].spot;
}

int TaxiJobService::holderOfSpot(int spot) const
{
    if (spot < 0 || spot >= SPOT_COUNT)
    {
        return -1;
    }
    return m_spotHolder[spot];
}

int TaxiJobService::workerOfVehicle(int vehicleId) const
{
    if (vehicleId < 0)
    {
        return -1;
    }
    for (int p = 0; p < MAX_PLAYERS; ++p)
    {
        const State &state = m_state[p];
        if (state.vehicleId == vehicleId && (state.phase == Phase::Boarding || state.phase == Phase::Working))
        {
            return p;
        }
    }
    return -1;
}

const TaxiJobService::Ride *TaxiJobService::rideOf(int driverId) const
{
    if (!validId(driverId) || m_state[driverId].phase == Phase::NotWorking)
    {
        return nullptr;
    }
    return &m_state[driverId].ride;
}

int TaxiJobService::driverOfPassenger(int passengerId) const
{
    if (!validId(passengerId))
    {
        return -1;
    }
    for (int p = 0; p < MAX_PLAYERS; ++p)
    {
        if (m_state[p].ride.passengerId == passengerId)
        {
            return p;
        }
    }
    return -1;
}

// ------------------------------------------------------------------ смена

TaxiJobService::StartOutcome TaxiJobService::startWork(int playerId, const SpotUsable &usable)
{
    if (!validId(playerId))
    {
        return {StartResult::AlreadyWorking};
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::NotWorking)
    {
        return {StartResult::AlreadyWorking};
    }

    const int spot = firstFreeSpot(usable);
    if (spot >= 0)
    {
        state = State{};
        state.phase = Phase::Boarding;
        state.spot = spot;
        m_spotHolder[spot] = playerId;
        return {StartResult::Boarding, spot, 0};
    }

    state = State{};
    state.phase = Phase::Queued;
    m_queue.push_back(playerId);
    return {StartResult::Queued, -1, static_cast<int>(m_queue.size())};
}

void TaxiJobService::setVehicle(int playerId, int vehicleId)
{
    if (!validId(playerId))
    {
        return;
    }
    m_state[playerId].vehicleId = vehicleId;
}

void TaxiJobService::completeBoarding(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::Boarding)
    {
        return;
    }
    state.phase = Phase::Working;
    releaseSpot(playerId); // машина уехала — точка свободна для очереди
}

void TaxiJobService::requeueTail(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    releaseSpot(playerId);
    State &state = m_state[playerId];
    state.vehicleId = -1; // машину деспавнит привод
    removeFromQueue(playerId);
    state.phase = Phase::Queued;
    m_queue.push_back(playerId);
}

TaxiJobService::Promotion TaxiJobService::promoteQueue(const SpotUsable &usable)
{
    if (m_queue.empty())
    {
        return {};
    }
    const int spot = firstFreeSpot(usable);
    if (spot < 0)
    {
        return {};
    }

    const int playerId = m_queue.front();
    m_queue.pop_front();

    State &state = m_state[playerId];
    state = State{};
    state.phase = Phase::Boarding;
    state.spot = spot;
    m_spotHolder[spot] = playerId;
    return {playerId, spot};
}

// ------------------------------------------------------------------ поездка

bool TaxiJobService::setPassenger(int driverId, int passengerId)
{
    if (!validId(driverId) || !validId(passengerId) || driverId == passengerId)
    {
        return false;
    }
    State &state = m_state[driverId];
    if (state.phase != Phase::Working || state.ride.passengerId >= 0)
    {
        return false;
    }
    state.ride = Ride{};
    state.ride.passengerId = passengerId;
    return true;
}

bool TaxiJobService::setDestination(int driverId, const Vector3 &destination, std::string name)
{
    if (!validId(driverId))
    {
        return false;
    }
    Ride &ride = m_state[driverId].ride;
    if (ride.passengerId < 0)
    {
        return false;
    }
    ride.hasDestination = true;
    ride.destination = destination;
    ride.destinationName = std::move(name);
    if (!ride.paid)
    {
        ride.fare = 0; // не внесённая цена была за другой маршрут — договариваются заново
    }
    return true;
}

bool TaxiJobService::offerFare(int driverId, std::int64_t fare)
{
    if (!validId(driverId) || fare <= 0)
    {
        return false;
    }
    Ride &ride = m_state[driverId].ride;
    if (ride.passengerId < 0 || !ride.hasDestination || ride.paid)
    {
        return false;
    }
    ride.fare = fare;
    return true;
}

bool TaxiJobService::confirmFare(int driverId)
{
    if (!validId(driverId))
    {
        return false;
    }
    Ride &ride = m_state[driverId].ride;
    if (ride.passengerId < 0 || ride.fare <= 0 || ride.paid)
    {
        return false;
    }
    ride.paid = true;
    return true;
}

TaxiJobService::Ride TaxiJobService::endRide(int driverId)
{
    if (!validId(driverId))
    {
        return Ride{};
    }
    Ride finished = std::move(m_state[driverId].ride);
    m_state[driverId].ride = Ride{};
    return finished;
}

void TaxiJobService::endShift(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    releaseSpot(playerId);
    removeFromQueue(playerId);
    m_state[playerId] = State{};
}

// ------------------------------------------------------------------ private

int TaxiJobService::firstFreeSpot(const SpotUsable &usable) const
{
    for (int i = 0; i < SPOT_COUNT; ++i)
    {
        // Точка годится, только если она И не закреплена за работником, И физически
        // пуста: занятую чужой машиной пропускаем — иначе выдали бы машину в машину.
        if (m_spotHolder[i] < 0 && (!usable || usable(i)))
        {
            return i;
        }
    }
    return -1;
}

void TaxiJobService::removeFromQueue(int playerId)
{
    const auto it = std::find(m_queue.begin(), m_queue.end(), playerId);
    if (it != m_queue.end())
    {
        m_queue.erase(it);
    }
}

void TaxiJobService::releaseSpot(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    const int spot = m_state[playerId].spot;
    if (spot >= 0 && spot < SPOT_COUNT && m_spotHolder[spot] == playerId)
    {
        m_spotHolder[spot] = -1;
    }
    m_state[playerId].spot = -1;
}
