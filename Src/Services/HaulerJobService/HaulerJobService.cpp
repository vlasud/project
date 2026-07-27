#include "Services/HaulerJobService/HaulerJobService.h"

#include <algorithm>

namespace
{
bool validId(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}
} // namespace

bool HaulerJobService::holdsVehicle(Phase phase)
{
    // Личный грузовик закреплён/ведётся во всех активных фазах, кроме очереди.
    return phase == Phase::Reserved || phase == Phase::DriveOut || phase == Phase::Loading ||
           phase == Phase::DriveBack || phase == Phase::Unloading;
}

HaulerJobService::Phase HaulerJobService::phaseOf(int playerId) const
{
    if (!validId(playerId))
    {
        return Phase::NotWorking;
    }
    return m_state[playerId].phase;
}

bool HaulerJobService::isWorking(int playerId) const
{
    return phaseOf(playerId) != Phase::NotWorking;
}

HaulerJobService::Role HaulerJobService::roleOf(int playerId) const
{
    if (!validId(playerId))
    {
        return Role::None;
    }
    return m_state[playerId].role;
}

int HaulerJobService::partnerOf(int playerId) const
{
    if (!validId(playerId))
    {
        return -1;
    }
    return m_state[playerId].partnerId;
}

int HaulerJobService::shiftOwnerOf(int playerId) const
{
    if (!validId(playerId))
    {
        return -1;
    }
    const State &state = m_state[playerId];
    if (state.role == Role::Driver)
    {
        return holdsVehicle(state.phase) ? playerId : -1;
    }
    if (state.role == Role::Loader && validId(state.partnerId))
    {
        return state.partnerId;
    }
    return -1;
}

int HaulerJobService::carrierOf(int driverId) const
{
    if (!validId(driverId))
    {
        return -1;
    }
    const int partner = m_state[driverId].partnerId;
    return validId(partner) ? partner : driverId;
}

int HaulerJobService::vehicleIdOf(int playerId) const
{
    if (!validId(playerId))
    {
        return -1;
    }
    return m_state[playerId].vehicleId;
}

int HaulerJobService::driveIndexOf(int playerId) const
{
    if (!validId(playerId))
    {
        return 0;
    }
    return m_state[playerId].driveIndex;
}

int HaulerJobService::boxCountOf(int playerId) const
{
    if (!validId(playerId))
    {
        return 0;
    }
    return m_state[playerId].boxCount;
}

bool HaulerJobService::carryingOf(int playerId) const
{
    if (!validId(playerId))
    {
        return false;
    }
    return m_state[playerId].carrying;
}

int HaulerJobService::queuePositionOf(int playerId) const
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

std::vector<int> HaulerJobService::queuedPlayers() const
{
    return std::vector<int>(m_queue.begin(), m_queue.end());
}

int HaulerJobService::standingVehicle(int spot) const
{
    if (spot < 0 || spot >= SLOT_COUNT)
    {
        return -1;
    }
    return m_spots[spot].vehicleId;
}

bool HaulerJobService::spotReserved(int spot) const
{
    if (spot < 0 || spot >= SLOT_COUNT)
    {
        return false;
    }
    return m_spots[spot].reservedBy >= 0;
}

int HaulerJobService::reservedSpotOf(int playerId) const
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

int HaulerJobService::workerOfVehicle(int vehicleId) const
{
    if (vehicleId < 0)
    {
        return -1;
    }
    for (int p = 0; p < MAX_PLAYERS; ++p)
    {
        const State &st = m_state[p];
        if (st.vehicleId == vehicleId && holdsVehicle(st.phase))
        {
            return p;
        }
    }
    return -1;
}

bool HaulerJobService::isFreeStandingVehicle(int vehicleId) const
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

int HaulerJobService::firstFreeStandingSpot() const
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

void HaulerJobService::removeFromQueue(int playerId)
{
    const auto it = std::find(m_queue.begin(), m_queue.end(), playerId);
    if (it != m_queue.end())
    {
        m_queue.erase(it);
    }
}

HaulerJobService::StartOutcome HaulerJobService::startWork(int playerId)
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
        m_spots[spot].reservedBy = playerId;
        state = State{};
        state.phase = Phase::Reserved;
        state.role = Role::Driver;
        state.vehicleId = m_spots[spot].vehicleId;
        return {StartResult::Reserved, state.vehicleId, 0};
    }

    state = State{};
    state.phase = Phase::Queued;
    state.role = Role::Driver;
    m_queue.push_back(playerId);
    return {StartResult::Queued, -1, static_cast<int>(m_queue.size())};
}

bool HaulerJobService::startLoader(int playerId)
{
    if (!validId(playerId))
    {
        return false;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::NotWorking)
    {
        return false;
    }
    state = State{};
    state.phase = Phase::Standby;
    state.role = Role::Loader;
    return true;
}

bool HaulerJobService::makePair(int driverId, int loaderId)
{
    if (!validId(driverId) || !validId(loaderId) || driverId == loaderId)
    {
        return false;
    }
    State &driver = m_state[driverId];
    State &loader = m_state[loaderId];
    // Водитель — с грузовиком (в очереди пары нет: обслуживать нечего) и без пары.
    if (driver.role != Role::Driver || !holdsVehicle(driver.phase) || driver.partnerId >= 0)
    {
        return false;
    }
    // Грузчик — устроен грузчиком и свободен.
    if (loader.role != Role::Loader || loader.phase != Phase::Standby || loader.partnerId >= 0)
    {
        return false;
    }
    driver.partnerId = loaderId;
    loader.partnerId = driverId;
    return true;
}

int HaulerJobService::breakPair(int playerId)
{
    if (!validId(playerId))
    {
        return -1;
    }
    const int partner = m_state[playerId].partnerId;
    m_state[playerId].partnerId = -1;
    if (validId(partner) && m_state[partner].partnerId == playerId)
    {
        m_state[partner].partnerId = -1;
    }
    return partner;
}

void HaulerJobService::clearCarry(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    m_state[playerId].carrying = false;
}

void HaulerJobService::completeBoarding(int playerId)
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
    state.phase = Phase::DriveOut;
    // Грузовик покинул депо — освободить площадку (state.vehicleId остаётся у игрока).
    const int spot = reservedSpotOf(playerId);
    if (spot >= 0)
    {
        m_spots[spot] = Spot{};
    }
}

int HaulerJobService::advanceDrive(int playerId)
{
    if (!validId(playerId))
    {
        return 0;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::DriveOut && state.phase != Phase::DriveBack)
    {
        return state.driveIndex;
    }
    ++state.driveIndex;
    return state.driveIndex;
}

void HaulerJobService::beginLoading(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::DriveOut)
    {
        return;
    }
    state.phase = Phase::Loading;
    state.boxCount = 0;
    state.carrying = false;
}

void HaulerJobService::beginDriveBack(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::Loading)
    {
        return;
    }
    state.phase = Phase::DriveBack;
    state.driveIndex = 0;
    state.carrying = false;
}

void HaulerJobService::beginUnloading(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::DriveBack)
    {
        return;
    }
    state.phase = Phase::Unloading;
    state.boxCount = 0;
    state.carrying = false;
}

void HaulerJobService::completeCycle(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::Unloading)
    {
        return;
    }
    state.phase = Phase::DriveOut;
    state.driveIndex = 0;
    state.boxCount = 0;
    state.carrying = false;
}

void HaulerJobService::beginCarry(int carrierId)
{
    const int owner = shiftOwnerOf(carrierId);
    if (owner < 0 || carrierOf(owner) != carrierId)
    {
        return; // не носильщик этой смены (водитель в паре коробки не берёт)
    }
    const Phase phase = m_state[owner].phase;
    if (phase != Phase::Loading && phase != Phase::Unloading)
    {
        return;
    }
    m_state[carrierId].carrying = true;
}

int HaulerJobService::finishCarry(int carrierId)
{
    const int owner = shiftOwnerOf(carrierId);
    if (owner < 0 || carrierOf(owner) != carrierId)
    {
        return 0;
    }
    State &carrier = m_state[carrierId];
    State &shift = m_state[owner];
    if (!carrier.carrying || (shift.phase != Phase::Loading && shift.phase != Phase::Unloading))
    {
        return 0;
    }
    carrier.carrying = false;
    ++shift.boxCount; // прогресс плеча — у смены (водителя), а не у носильщика
    return shift.boxCount;
}

void HaulerJobService::setStanding(int spot, int vehicleId)
{
    if (spot < 0 || spot >= SLOT_COUNT)
    {
        return;
    }
    m_spots[spot].vehicleId = vehicleId;
    m_spots[spot].reservedBy = -1;
}

void HaulerJobService::clearStanding(int spot)
{
    if (spot < 0 || spot >= SLOT_COUNT)
    {
        return;
    }
    m_spots[spot] = Spot{};
}

HaulerJobService::Promotion HaulerJobService::promoteQueue()
{
    if (m_queue.empty())
    {
        return {};
    }
    const int spot = firstFreeStandingSpot();
    if (spot < 0)
    {
        return {};
    }

    const int playerId = m_queue.front();
    m_queue.pop_front();

    m_spots[spot].reservedBy = playerId;
    State &state = m_state[playerId];
    state = State{};
    state.phase = Phase::Reserved;
    state.role = Role::Driver; // State{} стирает роль — восстанавливаем явно
    state.vehicleId = m_spots[spot].vehicleId;
    return {playerId, state.vehicleId};
}

void HaulerJobService::detachVehicle(int playerId, bool busKept)
{
    // Reserved: у игрока закреплён грузовик на площадке — оставить pre-stock
    // (busKept) либо освободить площадку. Прочие активные фазы: грузовик личный
    // ведомый (площадки нет) — привод его деспавнит.
    const int spot = reservedSpotOf(playerId);
    if (spot >= 0)
    {
        if (busKept)
        {
            m_spots[spot].reservedBy = -1;
        }
        else
        {
            m_spots[spot] = Spot{};
        }
    }
    State &state = m_state[playerId];
    state.vehicleId = -1;
    state.driveIndex = 0;
    state.boxCount = 0;
    state.carrying = false;
}

void HaulerJobService::requeueTail(int playerId, bool busKept)
{
    if (!validId(playerId))
    {
        return;
    }
    breakPair(playerId); // грузовика нет — обслуживать грузчику нечего
    detachVehicle(playerId, busKept);
    removeFromQueue(playerId);
    m_state[playerId].phase = Phase::Queued;
    m_state[playerId].role = Role::Driver;
    m_queue.push_back(playerId);
}

void HaulerJobService::endShift(int playerId, bool busKept)
{
    if (!validId(playerId))
    {
        return;
    }
    breakPair(playerId); // пара не переживает конец смены ни одной из сторон
    detachVehicle(playerId, busKept);
    removeFromQueue(playerId);
    m_state[playerId] = State{};
}
