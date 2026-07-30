#include "Services/PhoneService/PhoneService.h"

#include <cstddef>
#include <utility>

namespace
{
bool validId(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}

const std::string EMPTY_NAME;

// Сколько протухших дозвонов снимается за один тик. Звонков в игре единицы, а
// остаток заберёт следующий тик — обход и уведомление остаются ограниченными.
constexpr std::size_t EXPIRE_BATCH = 32;
} // namespace

bool PhoneService::validService(Service service)
{
    return service >= Service::Ambulance && service < Service::Count;
}

void PhoneService::registerDispatch(Service service, std::string callName, DispatchEnumerator enumerate)
{
    if (!validService(service) || !enumerate)
    {
        return;
    }
    Dispatch &dispatch = m_dispatch[static_cast<std::size_t>(service)];
    dispatch.callName = std::move(callName);
    dispatch.enumerate = std::move(enumerate);
}

const std::string &PhoneService::callNameOf(Service service) const
{
    if (!validService(service))
    {
        return EMPTY_NAME;
    }
    return m_dispatch[static_cast<std::size_t>(service)].callName;
}

void PhoneService::forEachWorker(Service service, const WorkerVisitor &visitor) const
{
    if (!validService(service) || !visitor)
    {
        return;
    }
    const Dispatch &dispatch = m_dispatch[static_cast<std::size_t>(service)];
    if (dispatch.enumerate)
    {
        dispatch.enumerate(visitor);
    }
}

bool PhoneService::hasDispatch(Service service) const
{
    return validService(service) && static_cast<bool>(m_dispatch[static_cast<std::size_t>(service)].enumerate);
}

// ------------------------------------------------------------------ номера

std::int64_t PhoneService::phoneOf(int playerId) const
{
    if (!validId(playerId))
    {
        return NO_PHONE;
    }
    return m_phone[playerId];
}

bool PhoneService::hasPhone(int playerId) const
{
    return phoneOf(playerId) != NO_PHONE;
}

int PhoneService::playerByPhone(std::int64_t phone) const
{
    if (phone == NO_PHONE)
    {
        return -1;
    }
    // Номеров онлайн — по числу игроков, поиск идёт по команде игрока (холодный путь).
    for (int p = 0; p < MAX_PLAYERS; ++p)
    {
        if (m_phone[p] == phone)
        {
            return p;
        }
    }
    return -1;
}

void PhoneService::setPhone(int playerId, std::int64_t phone)
{
    if (!validId(playerId))
    {
        return;
    }
    m_phone[playerId] = phone;
}

std::int64_t PhoneService::balanceOf(int playerId) const
{
    if (!validId(playerId))
    {
        return 0;
    }
    return m_balance[playerId];
}

void PhoneService::setBalance(int playerId, std::int64_t balance)
{
    if (!validId(playerId))
    {
        return;
    }
    // Отрицательного счёта не бывает: битая строка в БД не должна дать «долг».
    m_balance[playerId] = balance > 0 ? balance : 0;
}

bool PhoneService::takeBalance(int playerId, std::int64_t amount)
{
    if (!validId(playerId) || amount <= 0 || m_balance[playerId] < amount)
    {
        return false;
    }
    m_balance[playerId] -= amount;
    return true;
}

int PhoneService::resetPlayer(int playerId)
{
    if (!validId(playerId))
    {
        return -1;
    }
    m_phone[playerId] = NO_PHONE;
    m_balance[playerId] = 0;
    cancelOrdersOf(playerId);
    m_numberRequest[playerId] = false;
    m_recall[playerId] = Recall{};
    return hangup(playerId); // звонок снимается парой; peerId — кому сообщить
}

bool PhoneService::beginNumberRequest(int playerId)
{
    if (!validId(playerId) || m_numberRequest[playerId])
    {
        return false;
    }
    m_numberRequest[playerId] = true;
    return true;
}

void PhoneService::endNumberRequest(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    m_numberRequest[playerId] = false;
}

// ------------------------------------------------------------------ звонки

void PhoneService::clearLink(int playerId)
{
    if (!validId(playerId) || m_calls[playerId].state == CallState::None)
    {
        return;
    }
    m_calls[playerId] = CallLink{};
    --m_linkCount;
}

bool PhoneService::paired(int playerId) const
{
    if (!validId(playerId))
    {
        return false;
    }
    const int peerId = m_calls[playerId].peerId;
    return validId(peerId) && m_calls[peerId].state != CallState::None && m_calls[peerId].peerId == playerId;
}

bool PhoneService::startCall(int callerId, std::uint32_t callerSerial, std::int64_t callerPhone, int calleeId,
                             std::uint32_t calleeSerial, std::int64_t calleePhone, TimePoint now)
{
    // Звонок самому себе запрещён В СЕРВИСЕ: симметричная запись пары затёрла бы
    // сама себя и оставила неснимаемую половину.
    if (!validId(callerId) || !validId(calleeId) || callerId == calleeId)
    {
        return false;
    }
    if (m_calls[callerId].state != CallState::None || m_calls[calleeId].state != CallState::None)
    {
        return false; // одна сторона занята — больше одного звонка на игрока нет
    }

    CallLink &outgoing = m_calls[callerId];
    outgoing.state = CallState::Outgoing;
    outgoing.peerId = calleeId;
    outgoing.peerSerial = calleeSerial;
    outgoing.peerPhone = calleePhone;
    outgoing.startedAt = now;
    outgoing.initiator = true; // набирал он — он и платит за разговор

    CallLink &incoming = m_calls[calleeId];
    incoming.state = CallState::Incoming;
    incoming.peerId = callerId;
    incoming.peerSerial = callerSerial;
    incoming.peerPhone = callerPhone;
    incoming.startedAt = now;

    m_linkCount += 2;
    m_recall[callerId].phone = calleePhone;
    m_recall[callerId].at = now;
    return true;
}

int PhoneService::answer(int playerId, TimePoint now)
{
    if (!validId(playerId) || m_calls[playerId].state != CallState::Incoming)
    {
        return -1;
    }
    const int peerId = m_calls[playerId].peerId;
    if (!paired(playerId) || m_calls[peerId].state != CallState::Outgoing)
    {
        // Пара рассогласована — снимаем звонок целиком, а не половину.
        clearLink(playerId);
        clearLink(peerId);
        return -1;
    }
    m_calls[playerId].state = CallState::Active;
    m_calls[playerId].startedAt = now;
    m_calls[peerId].state = CallState::Active;
    m_calls[peerId].startedAt = now;
    // Тариф отсчитывается от МОМЕНТА ОТВЕТА: дозвон бесплатен, платит разговор.
    m_calls[playerId].chargedAt = now;
    m_calls[peerId].chargedAt = now;
    return peerId;
}

int PhoneService::hangup(int playerId)
{
    if (!validId(playerId) || m_calls[playerId].state == CallState::None)
    {
        return -1;
    }
    const int peerId = m_calls[playerId].peerId;
    const bool hasPeer = paired(playerId);
    clearLink(playerId);
    if (!hasPeer)
    {
        return -1;
    }
    clearLink(peerId);
    return peerId;
}

const PhoneService::CallLink *PhoneService::callOf(int playerId) const
{
    if (!validId(playerId) || m_calls[playerId].state == CallState::None)
    {
        return nullptr;
    }
    return &m_calls[playerId];
}

int PhoneService::recallSecondsLeft(int playerId, std::int64_t phone, TimePoint now) const
{
    if (!validId(playerId) || phone == NO_PHONE || m_recall[playerId].phone != phone)
    {
        return 0;
    }
    const auto passed = now - m_recall[playerId].at;
    if (passed >= RECALL_COOLDOWN)
    {
        return 0;
    }
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(RECALL_COOLDOWN - passed).count()) + 1;
}

void PhoneService::expireRinging(TimePoint now, const RingVisitor &visitor)
{
    if (m_linkCount <= 0)
    {
        return; // звонков нет — массив не обходим вовсе
    }
    // Снятые пары копим и уведомляем ПОСЛЕ обхода: наблюдатель шлёт сообщения, и
    // видеть полуснятый звонок он не должен.
    std::array<ExpiredRing, EXPIRE_BATCH> expired{};
    std::size_t count = 0;

    for (int id = 0; id < MAX_PLAYERS && count < EXPIRE_BATCH; ++id)
    {
        if (m_calls[id].state == CallState::None)
        {
            continue;
        }
        if (!paired(id))
        {
            clearLink(id); // осиротевшая половина: держать нечего
            continue;
        }
        if (m_calls[id].state != CallState::Outgoing || now - m_calls[id].startedAt < RING_TIMEOUT)
        {
            continue;
        }
        const int peerId = m_calls[id].peerId;
        // Серии снимаем ДО очистки: каждая половина хранит серию ПРОТИВОПОЛОЖНОЙ
        // стороны, поэтому своя серия лежит у собеседника.
        ExpiredRing &ring = expired[count++];
        ring.callerId = id;
        ring.callerSerial = m_calls[peerId].peerSerial;
        ring.calleeId = peerId;
        ring.calleeSerial = m_calls[id].peerSerial;
        clearLink(id);
        clearLink(peerId);
    }

    if (!visitor)
    {
        return;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        visitor(expired[i]);
    }
}

void PhoneService::collectCharges(TimePoint now, const ChargeVisitor &visitor)
{
    if (m_linkCount <= 0 || !visitor)
    {
        return; // разговоров нет — массив не обходим вовсе
    }
    // Как и в expireRinging: сперва обход и сдвиг отметок, потом визитор. Он снимает
    // деньги и может оборвать звонок, а рвать связку во время обхода нельзя.
    std::array<CallCharge, EXPIRE_BATCH> due{};
    std::size_t count = 0;

    for (int id = 0; id < MAX_PLAYERS && count < EXPIRE_BATCH; ++id)
    {
        const CallLink &link = m_calls[id];
        if (link.state != CallState::Active || !link.initiator)
        {
            continue; // платит только набиравший, и только за состоявшийся разговор
        }
        if (!paired(id) || now - link.chargedAt < CALL_CHARGE_INTERVAL)
        {
            continue;
        }
        const int peerId = link.peerId;
        CallCharge &charge = due[count++];
        charge.payerId = id;
        // Своя серия лежит у собеседника: каждая половина хранит серию ПРОТИВОПОЛОЖНОЙ.
        charge.payerSerial = m_calls[peerId].peerSerial;
        charge.peerId = peerId;
        charge.peerSerial = link.peerSerial;
        // Отметку двигаем СРАЗУ: даже если визитор оборвёт звонок, повторного
        // списания за тот же интервал не будет.
        m_calls[id].chargedAt = now;
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        visitor(due[i]);
    }
}

// ------------------------------------------------------------------ заказы

void PhoneService::dropExpired(TimePoint now)
{
    for (auto it = m_orders.begin(); it != m_orders.end();)
    {
        if (now - it->second.createdAt >= ORDER_TTL)
        {
            it = m_orders.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

int PhoneService::createOrder(Service service, int callerId, std::uint32_t callerSerial, const Vector3 &position,
                              TimePoint now)
{
    if (!validService(service) || !validId(callerId))
    {
        return 0;
    }
    dropExpired(now);
    if (orderOfCaller(callerId, now) != nullptr)
    {
        return 0; // один живой заказ на игрока — иначе один звонящий спамит всем работникам
    }

    Order order;
    order.number = m_nextOrderNumber++;
    order.service = service;
    order.callerId = callerId;
    order.callerSerial = callerSerial;
    order.position = position;
    order.createdAt = now;

    const int number = order.number;
    m_orders[number] = std::move(order);
    return number;
}

bool PhoneService::acceptOrder(int number, Service service, TimePoint now, Order &out)
{
    const auto it = m_orders.find(number);
    if (it == m_orders.end())
    {
        return false;
    }
    if (it->second.service != service || now - it->second.createdAt >= ORDER_TTL)
    {
        return false; // чужая служба либо протух
    }
    // Заказ забирает ПЕРВЫЙ принявший: удаляем сразу, второму его уже не найти.
    out = it->second;
    m_orders.erase(it);
    return true;
}

const PhoneService::Order *PhoneService::orderOfCaller(int callerId, TimePoint now) const
{
    for (const auto &[number, order] : m_orders)
    {
        if (order.callerId == callerId && now - order.createdAt < ORDER_TTL)
        {
            return &order;
        }
    }
    return nullptr;
}

void PhoneService::cancelOrdersOf(int callerId)
{
    for (auto it = m_orders.begin(); it != m_orders.end();)
    {
        if (it->second.callerId == callerId)
        {
            it = m_orders.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
