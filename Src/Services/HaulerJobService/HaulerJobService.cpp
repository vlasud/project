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
    // Личный грузовик закреплён/ведётся во всех активных фазах, кроме очереди. Idle
    // сюда входит обязательно: без выбранной цели грузовик всё равно ЕГО, и гейт руля
    // опирается ровно на этот список.
    return phase == Phase::Reserved || phase == Phase::Idle || phase == Phase::DriveOut ||
           phase == Phase::Loading || phase == Phase::DriveBack || phase == Phase::Unloading;
}

HaulerJobService::Phase HaulerJobService::phaseOf(int playerId) const
{
    if (!validId(playerId))
    {
        return Phase::NotWorking;
    }
    return m_state[playerId].phase;
}

HaulerJobService::Mode HaulerJobService::modeOf(int playerId) const
{
    if (!validId(playerId))
    {
        return Mode::Port;
    }
    return m_state[playerId].mode;
}

int HaulerJobService::orderIdOf(int playerId) const
{
    if (!validId(playerId))
    {
        return 0;
    }
    return m_state[playerId].orderId;
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

int HaulerJobService::holderOfSpot(int spot) const
{
    if (spot < 0 || spot >= SLOT_COUNT)
    {
        return -1;
    }
    return m_spots[spot].reservedBy;
}

int HaulerJobService::truckCount() const
{
    // Площадки и «уехавшие» не пересекаются: площадку освобождает releaseSpot, когда
    // грузовик физически покидает её, поэтому одна машина считается ровно один раз.
    int count = 0;
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (m_spots[i].vehicleId >= 0)
        {
            ++count;
        }
    }
    for (int p = 0; p < MAX_PLAYERS; ++p)
    {
        const State &state = m_state[p];
        if (state.vehicleId >= 0 && state.phase != Phase::Reserved && holdsVehicle(state.phase))
        {
            ++count;
        }
    }
    return count;
}

int HaulerJobService::firstFreeSpot(const SpotUsable &usable) const
{
    // С КОНЦА ряда (справа налево): дальняя площадка отдаётся первой, ближние к
    // выезду остаются свободными дольше — выезжающий не продирается мимо чужих машин.
    for (int i = SLOT_COUNT - 1; i >= 0; --i)
    {
        // Площадка годится, только если она И ничья, И физически пуста: занятую
        // чужой машиной пропускаем — иначе выдали бы грузовик в машину.
        if (m_spots[i].reservedBy < 0 && m_spots[i].vehicleId < 0 && (!usable || usable(i)))
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

HaulerJobService::StartOutcome HaulerJobService::startWork(int playerId, const SpotUsable &usable)
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
    // Цель рейса на устройстве НЕ выбирается: сперва грузовик, дальше водитель за рулём
    // берёт порт или заказ (/target). Поэтому режим здесь дефолтный, а заказа нет.
    const int spot = firstFreeSpot(usable);
    if (spot >= 0)
    {
        m_spots[spot].reservedBy = playerId;
        state = State{};
        state.phase = Phase::Reserved;
        state.role = Role::Driver;
        // Грузовика ещё нет: его спавнит привод и регистрирует через setStanding.
        return {StartResult::Reserved, -1, 0};
    }

    state = State{};
    state.phase = Phase::Queued;
    state.role = Role::Driver;
    m_queue.push_back(playerId);
    return {StartResult::Queued, -1, static_cast<int>(m_queue.size())};
}

void HaulerJobService::clearOrder(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    m_state[playerId].orderId = 0;
    // Без заказа режим Orders ничего не значит: цель выбирается заново (/target).
    m_state[playerId].mode = Mode::Port;
}

bool HaulerJobService::startPortRun(int playerId)
{
    if (!validId(playerId))
    {
        return false;
    }
    State &state = m_state[playerId];
    // ТОЛЬКО из Idle: цель меняют без активной задачи, иначе водитель бросал бы
    // начатый рейс на полпути (и заказ вместе с ним).
    if (state.phase != Phase::Idle || state.role != Role::Driver || state.vehicleId < 0)
    {
        return false;
    }
    state.mode = Mode::Port;
    state.orderId = 0;
    state.phase = Phase::DriveOut;
    state.boxCount = 0;
    state.carrying = false;
    return true;
}

bool HaulerJobService::startOrderLeg(int playerId, int orderId)
{
    if (!validId(playerId) || orderId <= 0)
    {
        return false;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::Idle || state.role != Role::Driver || state.orderId != 0 || state.vehicleId < 0)
    {
        return false;
    }
    state.mode = Mode::Orders;
    state.orderId = orderId;
    // Груз заказа лежит на базе: рейс начинается сразу с погрузки, плеча в порт нет.
    state.phase = Phase::Loading;
    state.boxCount = 0;
    state.carrying = false;
    return true;
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
    // Сел за руль — цели рейса ещё нет: её выбирают уже здесь, /target. Площадку
    // отпускает привод (грузовик стоит на ней, но следующему из очереди она нужна).
    state.phase = Phase::Idle;
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

void HaulerJobService::finishRun(int playerId)
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
    // Рейс сдан. Грузовик остаётся за водителем, а цель следующего рейса он выбирает
    // заново (/target) — потому фаза Idle, а не новый круг.
    state.phase = Phase::Idle;
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
    // Площадка остаётся за работником (reservedBy не трогаем) — меняется только то,
    // какой грузовик на ней стоит.
    m_spots[spot].vehicleId = vehicleId;
}

void HaulerJobService::setVehicle(int playerId, int vehicleId)
{
    if (!validId(playerId))
    {
        return;
    }
    m_state[playerId].vehicleId = vehicleId;
}

void HaulerJobService::releaseSpot(int playerId)
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

HaulerJobService::Promotion HaulerJobService::promoteQueue(const SpotUsable &usable)
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

    m_spots[spot].reservedBy = playerId;
    // State{} стирает роль — восстанавливаем явно. Цели рейса у ждущего в очереди нет
    // (её выбирают за рулём), поэтому переносить нечего.
    m_state[playerId] = State{};
    m_state[playerId].phase = Phase::Reserved;
    m_state[playerId].role = Role::Driver;
    return {playerId, -1}; // грузовик спавнит привод
}

void HaulerJobService::detachVehicle(int playerId)
{
    // Площадка всегда освобождается: pre-stock грузовиков в депо нет, а личный
    // грузовик работника деспавнит привод.
    releaseSpot(playerId);
    State &state = m_state[playerId];
    state.vehicleId = -1;
    state.boxCount = 0;
    state.carrying = false;
}

void HaulerJobService::requeueTail(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    breakPair(playerId); // грузовика нет — обслуживать грузчику нечего
    detachVehicle(playerId);
    removeFromQueue(playerId);
    m_state[playerId].phase = Phase::Queued;
    m_state[playerId].role = Role::Driver;
    m_queue.push_back(playerId);
}

void HaulerJobService::endShift(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    breakPair(playerId); // пара не переживает конец смены ни одной из сторон
    detachVehicle(playerId);
    removeFromQueue(playerId);
    m_state[playerId] = State{};
}
