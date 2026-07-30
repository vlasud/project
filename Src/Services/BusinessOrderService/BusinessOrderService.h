#pragma once

#include "Services/IService.h"
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

class BusinessSystem;
class HaulerJobSystem;

// Заказы товара для бизнесов — мост между владельцем точки и работой развозчика.
// Источник правды о:
//  * составе заказа (что и сколько везём) и уплаченной премии развозчикам;
//  * прогрессе доставки в КОРОБКАХ (0..BOXES_PER_ORDER);
//  * том, кто заказ сейчас везёт (водитель смены) и какие заказы лежат в общем пуле.
//
// Отдельный сервис, а не поле бизнеса: заказ читают и меняют ДВЕ фичи — бизнес
// (создание, отмена, зачисление товара) и работа развозчика (пул, принятие,
// доставка). Спрятать его внутрь одной из них значило бы, что вторая лезет в чужое.
//
// Деньги сервис НЕ трогает: закупку и премию списывает бизнес при создании, оплату
// развозчикам начисляет работа. Здесь только состояние.
//
// Персист (business_order) делает BusinessSystem: заказ хранит уже СПИСАННЫЕ у
// владельца деньги, и пережить рестарт он обязан.
class BusinessOrderService final : public IService
{
    friend BusinessSystem;
    friend HaulerJobSystem;

  public:
    // На сколько коробок дробится ЛЮБОЙ заказ. Совпадает с плечом развозчика
    // (HaulerJobService::BOXES_PER_LEG): рейс за заказ — ровно одна ходка.
    static constexpr int BOXES_PER_ORDER = 10;

    // Потолок премии — доля от стоимости закупки. Без него владелец мог бы вбить
    // произвольную сумму и вывести деньги напарнику мимо экономики.
    static constexpr std::int64_t MAX_BONUS_PERCENT = 40;

    struct Item
    {
        int itemType = 0;
        int quantity = 0; // сколько ВСЕГО заказано этого товара
    };

    struct Order
    {
        int id = 0;
        int businessId = 0;
        std::int64_t bonus = 0; // делится между водителем и грузчиком по завершении
        int boxesDone = 0;      // сколько коробок уже выгружено в точку
        int driverId = -1;      // кто везёт прямо сейчас; -1 — лежит в пуле
        std::vector<Item> items;
    };

    // Сколько единиц товара даёт ОДНА коробка. Округление ВВЕРХ: 25 штук на 10
    // коробок — по 3, и последние коробки просто упрутся в остаток заказа.
    static int perBox(int totalQuantity);

    const Order *get(int orderId) const;
    // Свободные заказы, отсортированные по премии УБЫВАЮЩЕ (при равной — по id,
    // чтобы порядок не прыгал между открытиями списка).
    std::vector<const Order *> pool() const;
    // Заказ, который сейчас везёт этот водитель, либо nullptr.
    const Order *orderOfDriver(int driverId) const;
    // ВСЕ заказы точки — и лежащие в пуле, и те, что уже везут. Нужен, когда точка
    // перестаёт существовать (снос, отказ от бизнеса): снимать надо оба вида, иначе
    // заказ в доставке остался бы висеть на несуществующей точке. По id — порядок
    // обхода unordered_map не определён.
    std::vector<const Order *> ordersOf(int businessId) const;
    // Есть ли у точки незавершённый заказ (в пуле или в доставке).
    bool hasActiveOrder(int businessId) const;
    int boxesLeft(int orderId) const;
    // Сколько единиц товара привезёт СЛЕДУЮЩАЯ коробка: perBox, обрезанный остатком
    // заказа. Пусто — заказ уже довезён.
    std::vector<Item> nextBoxContents(int orderId) const;

  private:
    // --- вызывается только BusinessSystem / HaulerJobSystem ---

    // Создать заказ. Возвращает id (0 — состав пуст). id сквозной и переживает
    // рестарт (восстанавливается из загруженных заказов).
    int create(int businessId, std::vector<Item> items, std::int64_t bonus);
    void load(Order order); // из БД на старте; id сохраняется как есть

    bool accept(int orderId, int driverId); // взять из пула в доставку
    void release(int orderId);              // срыв доставки — обратно в пул как есть
    // Зачесть выгруженную коробку. true — это была ПОСЛЕДНЯЯ (заказ завершён).
    bool deliverBox(int orderId);
    void remove(int orderId); // отмена владельцем либо снос точки

    // Изменился состав пула либо прогресс заказа — привод пишет БД.
    using ChangedObserver = std::function<void(int orderId)>;
    void subscribeChanged(ChangedObserver observer);
    void notifyChanged(int orderId) const;

    Order *find(int orderId);

    std::unordered_map<int, Order> m_orders;
    int m_nextId = 1;
    std::vector<ChangedObserver> m_changedObservers;
};
