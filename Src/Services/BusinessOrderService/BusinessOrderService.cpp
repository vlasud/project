#include "Services/BusinessOrderService/BusinessOrderService.h"

#include "Utils/TimeFormat/TimeFormat.h"
#include <algorithm>

int BusinessOrderService::perBox(int totalQuantity)
{
    if (totalQuantity <= 0)
    {
        return 0;
    }
    // Округление ВВЕРХ: 25 на 10 коробок -> по 3. Последние коробки упрутся в
    // остаток заказа (см. nextBoxContents), поэтому лишнего не привезут.
    return (totalQuantity + BOXES_PER_ORDER - 1) / BOXES_PER_ORDER;
}

const BusinessOrderService::Order *BusinessOrderService::get(int orderId) const
{
    const auto it = m_orders.find(orderId);
    return it == m_orders.end() ? nullptr : &it->second;
}

BusinessOrderService::Order *BusinessOrderService::find(int orderId)
{
    const auto it = m_orders.find(orderId);
    return it == m_orders.end() ? nullptr : &it->second;
}

std::vector<const BusinessOrderService::Order *> BusinessOrderService::pool() const
{
    std::vector<const Order *> free;
    for (const auto &[id, order] : m_orders)
    {
        if (order.driverId < 0)
        {
            free.push_back(&order);
        }
    }
    // По премии УБЫВАЮЩЕ — самые выгодные сверху, как просили. При равной премии по
    // id: порядок обхода unordered_map не определён, и без второго ключа список
    // прыгал бы между открытиями.
    std::sort(free.begin(), free.end(),
              [](const Order *left, const Order *right)
              {
                  if (left->bonus != right->bonus)
                  {
                      return left->bonus > right->bonus;
                  }
                  return left->id < right->id;
              });
    return free;
}

const BusinessOrderService::Order *BusinessOrderService::orderOfDriver(int driverId) const
{
    if (driverId < 0)
    {
        return nullptr;
    }
    for (const auto &[id, order] : m_orders)
    {
        if (order.driverId == driverId)
        {
            return &order;
        }
    }
    return nullptr;
}

std::vector<const BusinessOrderService::Order *> BusinessOrderService::ordersOf(int businessId) const
{
    std::vector<const Order *> found;
    for (const auto &[id, order] : m_orders)
    {
        if (order.businessId == businessId)
        {
            found.push_back(&order);
        }
    }
    std::sort(found.begin(), found.end(),
              [](const Order *left, const Order *right)
              {
                  return left->id < right->id;
              });
    return found;
}

bool BusinessOrderService::hasActiveOrder(int businessId) const
{
    for (const auto &[id, order] : m_orders)
    {
        if (order.businessId == businessId)
        {
            return true;
        }
    }
    return false;
}

int BusinessOrderService::boxesLeft(int orderId) const
{
    const Order *order = get(orderId);
    return order ? std::max(0, BOXES_PER_ORDER - order->boxesDone) : 0;
}

std::vector<BusinessOrderService::Item> BusinessOrderService::nextBoxContents(int orderId) const
{
    std::vector<Item> contents;
    const Order *order = get(orderId);
    if (!order || order->boxesDone >= BOXES_PER_ORDER)
    {
        return contents;
    }
    for (const Item &item : order->items)
    {
        const int step = perBox(item.quantity);
        // Сколько этого товара УЖЕ довезли предыдущими коробками — остаток и есть
        // потолок текущей. Из-за округления вверх последние коробки могут привезти
        // меньше step либо ничего.
        const int already = std::min(item.quantity, step * order->boxesDone);
        const int now = std::min(step, item.quantity - already);
        if (now > 0)
        {
            contents.push_back(Item{item.itemType, now});
        }
    }
    return contents;
}

// ------------------------------------------------------------------ мутации

int BusinessOrderService::create(int businessId, std::vector<Item> items, std::int64_t bonus)
{
    // Пустой состав — не заказ: развозчику нечего везти, а деньги списаны зря.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const Item &item)
                               {
                                   return item.quantity <= 0;
                               }),
                items.end());
    if (items.empty())
    {
        return 0;
    }

    Order order;
    order.id = m_nextId++;
    order.businessId = businessId;
    order.bonus = std::max<std::int64_t>(bonus, 0);
    // Момент заказа — АБСОЛЮТНЫЙ (unix): владелец должен видеть его и после
    // рестарта, а строка БД это поле уже хранит (created_at).
    order.createdAt = TimeFormat::nowUnix();
    order.items = std::move(items);
    const int id = order.id;
    m_orders.emplace(id, std::move(order));
    notifyChanged(id);
    return id;
}

void BusinessOrderService::load(Order order)
{
    if (order.id <= 0 || order.items.empty())
    {
        return;
    }
    // Водитель — рантайм-состояние: после рестарта его нет, заказ лежит в пуле.
    // Прогресс в коробках при этом сохраняется, довезённое остаётся довезённым.
    order.driverId = -1;
    order.boxesDone = std::clamp(order.boxesDone, 0, BOXES_PER_ORDER);
    m_nextId = std::max(m_nextId, order.id + 1);
    const int id = order.id;
    m_orders.emplace(id, std::move(order));
}

bool BusinessOrderService::accept(int orderId, int driverId)
{
    Order *order = find(orderId);
    if (!order || driverId < 0 || order->driverId >= 0)
    {
        return false; // заказа нет либо его уже взяли
    }
    if (orderOfDriver(driverId))
    {
        return false; // один водитель — один заказ за рейс
    }
    order->driverId = driverId;
    notifyChanged(orderId);
    return true;
}

void BusinessOrderService::release(int orderId)
{
    Order *order = find(orderId);
    if (!order || order->driverId < 0)
    {
        return;
    }
    // Срыв доставки штрафа не несёт: заказ возвращается в пул КАК ЕСТЬ, вместе с
    // премией и уже засчитанными коробками. Довезённое остаётся у точки, повторно
    // его не привезут — nextBoxContents считает от boxesDone.
    order->driverId = -1;
    notifyChanged(orderId);
}

bool BusinessOrderService::deliverBox(int orderId)
{
    Order *order = find(orderId);
    if (!order || order->boxesDone >= BOXES_PER_ORDER)
    {
        return false;
    }
    ++order->boxesDone;
    const bool finished = order->boxesDone >= BOXES_PER_ORDER;
    notifyChanged(orderId);
    return finished;
}

void BusinessOrderService::remove(int orderId)
{
    if (m_orders.erase(orderId) > 0)
    {
        notifyChanged(orderId); // привод сотрёт строку в БД
    }
}

void BusinessOrderService::subscribeChanged(ChangedObserver observer)
{
    if (observer)
    {
        m_changedObservers.push_back(std::move(observer));
    }
}

void BusinessOrderService::notifyChanged(int orderId) const
{
    for (const ChangedObserver &observer : m_changedObservers)
    {
        observer(orderId);
    }
}
