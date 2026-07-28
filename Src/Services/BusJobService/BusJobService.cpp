#include "Services/BusJobService/BusJobService.h"

#include <algorithm>

namespace
{
bool validId(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}
} // namespace

BusJobService::Phase BusJobService::phaseOf(int playerId) const
{
    if (!validId(playerId))
    {
        return Phase::NotWorking;
    }
    return m_state[playerId].phase;
}

bool BusJobService::isWorking(int playerId) const
{
    return phaseOf(playerId) != Phase::NotWorking;
}

int BusJobService::vehicleIdOf(int playerId) const
{
    if (!validId(playerId))
    {
        return -1;
    }
    return m_state[playerId].vehicleId;
}

int BusJobService::checkpointIndexOf(int playerId) const
{
    if (!validId(playerId))
    {
        return 0;
    }
    return m_state[playerId].cpIndex;
}

int BusJobService::queuePositionOf(int playerId) const
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
    return 0; // не в очереди
}

std::vector<int> BusJobService::queuedPlayers() const
{
    return std::vector<int>(m_queue.begin(), m_queue.end());
}

int BusJobService::standingVehicle(int spot) const
{
    if (spot < 0 || spot >= SLOT_COUNT)
    {
        return -1;
    }
    return m_spots[spot].vehicleId;
}

bool BusJobService::spotReserved(int spot) const
{
    if (spot < 0 || spot >= SLOT_COUNT)
    {
        return false;
    }
    return m_spots[spot].reservedBy >= 0;
}

int BusJobService::reservedSpotOf(int playerId) const
{
    if (playerId < 0)
    {
        return -1;
    }
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (m_spots[i].reservedBy == playerId)
        {
            return i;
        }
    }
    return -1;
}

int BusJobService::workerOfVehicle(int vehicleId) const
{
    if (vehicleId < 0)
    {
        return -1;
    }
    // Резервный/едущий автобус принадлежит игроку, чей state.vehicleId == id и фаза
    // Reserved/Driving. Pre-stock ничьей записи не занимает (state.vehicleId освобождён
    // при переходе в pre-stock) — сюда не попадёт.
    for (int p = 0; p < MAX_PLAYERS; ++p)
    {
        const State &st = m_state[p];
        if (st.vehicleId == vehicleId && (st.phase == Phase::Reserved || st.phase == Phase::Driving))
        {
            return p;
        }
    }
    return -1;
}

int BusJobService::holderOfSpot(int spot) const
{
    if (spot < 0 || spot >= SLOT_COUNT)
    {
        return -1;
    }
    return m_spots[spot].reservedBy;
}

int BusJobService::firstFreeSpot(const SpotUsable &usable) const
{
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        // Площадка годится, только если она И ничья, И физически пуста: занятую
        // чужой машиной пропускаем — иначе выдали бы автобус в машину.
        if (m_spots[i].reservedBy < 0 && m_spots[i].vehicleId < 0 && (!usable || usable(i)))
        {
            return i;
        }
    }
    return -1;
}

void BusJobService::removeFromQueue(int playerId)
{
    const auto it = std::find(m_queue.begin(), m_queue.end(), playerId);
    if (it != m_queue.end())
    {
        m_queue.erase(it);
    }
}

BusJobService::StartOutcome BusJobService::startWork(int playerId, const SpotUsable &usable)
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
        // Площадка закрепляется за игроком; автобус на неё подаёт привод (депо стоит
        // пустым) и регистрирует через setStanding/setVehicle.
        m_spots[spot].reservedBy = playerId;
        state.phase = Phase::Reserved;
        state.vehicleId = -1;
        state.cpIndex = 0;
        return {StartResult::Reserved, -1, 0};
    }

    // Свободных площадок нет — в конец FIFO.
    state.phase = Phase::Queued;
    state.vehicleId = -1;
    state.cpIndex = 0;
    m_queue.push_back(playerId);
    return {StartResult::Queued, -1, static_cast<int>(m_queue.size())};
}

void BusJobService::completeBoarding(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::Reserved)
    {
        return;
    }
    state.phase = Phase::Driving;
    // Площадку НЕ освобождаем: автобус всё ещё стоит на ней, пока работник не отъедет.
    // Освободит releaseSpot по факту отъезда (привод следит в тике).
}

int BusJobService::advanceCheckpoint(int playerId)
{
    if (!validId(playerId))
    {
        return 0;
    }
    State &state = m_state[playerId];
    state.cpIndex = (state.cpIndex + 1) % ROUTE_LENGTH;
    return state.cpIndex;
}

void BusJobService::setStanding(int spot, int vehicleId)
{
    if (spot < 0 || spot >= SLOT_COUNT)
    {
        return;
    }
    // Площадка остаётся за работником (reservedBy не трогаем) — меняется только то,
    // какой автобус на ней стоит.
    m_spots[spot].vehicleId = vehicleId;
}

void BusJobService::setVehicle(int playerId, int vehicleId)
{
    if (!validId(playerId))
    {
        return;
    }
    m_state[playerId].vehicleId = vehicleId;
}

void BusJobService::releaseSpot(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    const int spot = reservedSpotOf(playerId);
    if (spot >= 0)
    {
        m_spots[spot] = Spot{};
    }
}

BusJobService::Promotion BusJobService::promoteQueue(const SpotUsable &usable)
{
    if (m_queue.empty())
    {
        return {};
    }
    const int spot = firstFreeSpot(usable);
    if (spot < 0)
    {
        return {}; // свободных площадок нет — двигать некуда
    }

    const int playerId = m_queue.front();
    m_queue.pop_front();

    m_spots[spot].reservedBy = playerId;
    State &state = m_state[playerId];
    state.phase = Phase::Reserved;
    state.vehicleId = m_spots[spot].vehicleId;
    state.cpIndex = 0;
    return {playerId, state.vehicleId};
}

void BusJobService::detachBus(int playerId)
{
    // Площадка всегда освобождается: pre-stock автобусов в депо нет, а личный автобус
    // работника деспавнит привод.
    releaseSpot(playerId);
    State &state = m_state[playerId];
    state.vehicleId = -1;
    state.cpIndex = 0;
}

void BusJobService::requeueTail(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    detachBus(playerId);
    removeFromQueue(playerId);
    m_state[playerId].phase = Phase::Queued;
    m_queue.push_back(playerId);
}

void BusJobService::endShift(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    detachBus(playerId);
    removeFromQueue(playerId);
    m_state[playerId] = State{}; // NotWorking + чистые поля
}
