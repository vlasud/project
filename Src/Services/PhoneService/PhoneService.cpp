#include "Services/PhoneService/PhoneService.h"

#include <utility>

namespace
{
bool validId(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}

const std::string EMPTY_NAME;
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

void PhoneService::resetPlayer(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    m_phone[playerId] = NO_PHONE;
    cancelOrdersOf(playerId);
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
