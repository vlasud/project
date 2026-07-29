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

bool MedicJobService::canHeal(AccountId patientAccount, TimePoint now) const
{
    return healCooldownLeft(patientAccount, now) == 0;
}

int MedicJobService::healCooldownLeft(AccountId patientAccount, TimePoint now) const
{
    const auto it = m_healedAt.find(patientAccount);
    if (patientAccount <= 0 || it == m_healedAt.end())
    {
        return 0; // этот аккаунт ещё не лечили
    }
    const TimePoint healedAt = it->second;
    const auto elapsed = now - healedAt;
    if (elapsed >= HEAL_COOLDOWN)
    {
        return 0;
    }
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(HEAL_COOLDOWN - elapsed).count();
    return static_cast<int>(left) + 1; // округляем вверх: «осталось 0 секунд» не показываем
}

// ------------------------------------------------------------------ мутаторы

MedicJobService::StartOutcome MedicJobService::startWork(int playerId, const SpotUsable &usable)
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

MedicJobService::Promotion MedicJobService::promoteQueue(const SpotUsable &usable)
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

void MedicJobService::markHealed(AccountId patientAccount, TimePoint now)
{
    if (patientAccount <= 0)
    {
        return;
    }
    // Просроченные записи снимаем здесь же. Ключ — аккаунт, а аккаунты за аптайм не
    // кончаются: без чистки карта росла бы всю работу сервера. Лечение — холодный
    // путь (команда врача), проход по десяткам записей на нём дешевле отдельного
    // таймера. Снятая запись неотличима от «не лечили»: healCooldownLeft на обеих
    // отдаёт 0.
    for (auto it = m_healedAt.begin(); it != m_healedAt.end();)
    {
        if (now - it->second >= HEAL_COOLDOWN)
        {
            it = m_healedAt.erase(it);
        }
        else
        {
            ++it;
        }
    }
    m_healedAt[patientAccount] = now;
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

// ------------------------------------------------------------------ private

int MedicJobService::firstFreeSpot(const SpotUsable &usable) const
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
