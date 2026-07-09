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

bool BusJobService::isFreeStandingVehicle(int vehicleId) const
{
    if (vehicleId < 0)
    {
        return false;
    }
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (m_spots[i].vehicleId == vehicleId && m_spots[i].reservedBy < 0)
        {
            return true;
        }
    }
    return false;
}

int BusJobService::firstFreeStandingSpot() const
{
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (m_spots[i].vehicleId >= 0 && m_spots[i].reservedBy < 0)
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

BusJobService::StartOutcome BusJobService::startWork(int playerId)
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

    const int spot = firstFreeStandingSpot();
    if (spot >= 0)
    {
        // Есть свободный стоящий автобус — закрепить его за игроком (не спавним, он уже стоит).
        m_spots[spot].reservedBy = playerId;
        state.phase = Phase::Reserved;
        state.vehicleId = m_spots[spot].vehicleId;
        state.cpIndex = 0;
        return {StartResult::Reserved, state.vehicleId, 0};
    }

    // Свободных стоящих автобусов нет — в конец FIFO.
    state.phase = Phase::Queued;
    state.vehicleId = -1;
    state.cpIndex = 0;
    m_queue.push_back(playerId);
    return {StartResult::Queued, -1, static_cast<int>(m_queue.size())};
}

bool BusJobService::transferReservation(int playerId, int vehicleId)
{
    if (!validId(playerId) || vehicleId < 0)
    {
        return false;
    }
    if (m_state[playerId].phase != Phase::Reserved)
    {
        return false;
    }
    // Резервный автобус игрока УЖЕ уведён с его площадки (detached reserved: площадку
    // отвязал пере-сток, но state.vehicleId держит тот автобус) — перепривязка на ДРУГОЙ
    // стоящий осиротила бы прежний (нет ни площадки, ни стейта -> вечная утечка машины).
    // В СВОЙ уведённый автобус игрока пускает workerOfVehicle-ветка гейта, сюда он не
    // доходит; в чужой стоящий — отказ (пусть вернётся в свой либо провалит посадку).
    const int prev = reservedSpotOf(playerId);
    if (prev < 0)
    {
        return false;
    }
    // Целевой автобус должен быть свободным стоящим (не закреплённым).
    int target = -1;
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (m_spots[i].vehicleId == vehicleId && m_spots[i].reservedBy < 0)
        {
            target = i;
            break;
        }
    }
    if (target < 0)
    {
        return false;
    }
    // Освободить прежнюю площадку резерва (её автобус снова свободный pre-stock).
    m_spots[prev].reservedBy = -1;
    // Закрепить целевую за игроком. cpIndex не трогаем (посадка ещё не завершена).
    m_spots[target].reservedBy = playerId;
    m_state[playerId].vehicleId = vehicleId;
    return true;
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
    // Автобус покинул депо (стал личным едущим) — освободить его площадку, чтобы она
    // пере-стокнулась, как только физически чиста. state.vehicleId остаётся у игрока.
    const int spot = reservedSpotOf(playerId);
    if (spot >= 0)
    {
        m_spots[spot] = Spot{};
    }
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
    m_spots[spot].vehicleId = vehicleId;
    m_spots[spot].reservedBy = -1;
}

void BusJobService::clearStanding(int spot)
{
    if (spot < 0 || spot >= SLOT_COUNT)
    {
        return;
    }
    m_spots[spot] = Spot{};
}

BusJobService::Promotion BusJobService::promoteQueue()
{
    if (m_queue.empty())
    {
        return {};
    }
    const int spot = firstFreeStandingSpot();
    if (spot < 0)
    {
        return {}; // свободных стоящих автобусов нет — двигать некуда
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

void BusJobService::detachBus(int playerId, bool busKept)
{
    // Reserved: у игрока закреплён автобус на площадке — либо оставить его pre-stock
    // (busKept), либо освободить площадку. Driving: площадки нет (автобус личный
    // едущий) — привод его деспавнит. Прочие фазы — площадки/автобуса нет.
    const int spot = reservedSpotOf(playerId);
    if (spot >= 0)
    {
        if (busKept)
        {
            m_spots[spot].reservedBy = -1; // автобус остаётся стоять свободным pre-stock
        }
        else
        {
            m_spots[spot] = Spot{}; // площадка пуста — пере-стокуется приводом
        }
    }
    State &state = m_state[playerId];
    state.vehicleId = -1;
    state.cpIndex = 0;
}

void BusJobService::requeueTail(int playerId, bool busKept)
{
    if (!validId(playerId))
    {
        return;
    }
    detachBus(playerId, busKept);
    removeFromQueue(playerId);
    m_state[playerId].phase = Phase::Queued;
    m_queue.push_back(playerId);
}

void BusJobService::endShift(int playerId, bool busKept)
{
    if (!validId(playerId))
    {
        return;
    }
    detachBus(playerId, busKept);
    removeFromQueue(playerId);
    m_state[playerId] = State{}; // NotWorking + чистые поля
}
