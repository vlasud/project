#include "Services/MedicJobService/MedicJobService.h"

#include <algorithm>

namespace
{
bool validId(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}
} // namespace

MedicJobService::MedicJobService()
{
    m_spotHolder.fill(-1); // 0 — валидный playerId, «свободно» это -1
}

MedicJobService::Phase MedicJobService::phaseOf(int playerId) const
{
    if (!validId(playerId))
    {
        return Phase::NotWorking;
    }
    return m_state[playerId].phase;
}

bool MedicJobService::isWorking(int playerId) const
{
    return phaseOf(playerId) != Phase::NotWorking;
}

int MedicJobService::vehicleIdOf(int playerId) const
{
    if (!validId(playerId))
    {
        return -1;
    }
    return m_state[playerId].vehicleId;
}

int MedicJobService::queuePositionOf(int playerId) const
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

std::vector<int> MedicJobService::queuedPlayers() const
{
    return std::vector<int>(m_queue.begin(), m_queue.end());
}

int MedicJobService::spotOf(int playerId) const
{
    if (!validId(playerId))
    {
        return -1;
    }
    return m_state[playerId].spot;
}

int MedicJobService::holderOfSpot(int spot) const
{
    if (spot < 0 || spot >= SPOT_COUNT)
    {
        return -1;
    }
    return m_spotHolder[spot];
}

int MedicJobService::workerOfVehicle(int vehicleId) const
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

bool MedicJobService::canHeal(int patientId, TimePoint now) const
{
    return healCooldownLeft(patientId, now) == 0;
}

int MedicJobService::healCooldownLeft(int patientId, TimePoint now) const
{
    if (!validId(patientId))
    {
        return 0;
    }
    const TimePoint healedAt = m_healedAt[patientId];
    if (healedAt.time_since_epoch().count() == 0)
    {
        return 0; // ещё не лечили в этой сессии
    }
    const auto elapsed = now - healedAt;
    if (elapsed >= HEAL_COOLDOWN)
    {
        return 0;
    }
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(HEAL_COOLDOWN - elapsed).count();
    return static_cast<int>(left) + 1; // округляем вверх: «осталось 0 секунд» не показываем
}

// ------------------------------------------------------------------ мутаторы

MedicJobService::StartOutcome MedicJobService::startWork(int playerId)
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

    const int spot = firstFreeSpot();
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

void MedicJobService::setVehicle(int playerId, int vehicleId)
{
    if (!validId(playerId))
    {
        return;
    }
    m_state[playerId].vehicleId = vehicleId;
}

void MedicJobService::completeBoarding(int playerId)
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

void MedicJobService::requeueTail(int playerId)
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

MedicJobService::Promotion MedicJobService::promoteQueue()
{
    if (m_queue.empty())
    {
        return {};
    }
    const int spot = firstFreeSpot();
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

void MedicJobService::markHealed(int patientId, TimePoint now)
{
    if (!validId(patientId))
    {
        return;
    }
    m_healedAt[patientId] = now;
}

void MedicJobService::endShift(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    releaseSpot(playerId);
    removeFromQueue(playerId);
    m_state[playerId] = State{};
}

void MedicJobService::resetPatient(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    m_healedAt[playerId] = TimePoint{};
}

// ------------------------------------------------------------------ private

int MedicJobService::firstFreeSpot() const
{
    for (int i = 0; i < SPOT_COUNT; ++i)
    {
        if (m_spotHolder[i] < 0)
        {
            return i;
        }
    }
    return -1;
}

void MedicJobService::removeFromQueue(int playerId)
{
    const auto it = std::find(m_queue.begin(), m_queue.end(), playerId);
    if (it != m_queue.end())
    {
        m_queue.erase(it);
    }
}

void MedicJobService::releaseSpot(int playerId)
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
