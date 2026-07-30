#include "Services/BusinessService/BusinessService.h"

#include "Log/LogManager.h"
#include "Utils/Geometry/Geometry.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <utility>

namespace
{
// Точка выхода — за спиной создателя, чтобы выйдя он не стоял в пикапе входа.
// Два метра за спину создателя: там игрок появляется, выходя из бизнеса. Дистанция
// подобрана владельцем так, чтобы вышедший не остался стоять во входном пикапе
// (см. Docs/Business.md, «Риск ре-триггера»).
constexpr float EXIT_BEHIND = 2.0f;

const std::string EMPTY_NAME;
const std::vector<BusinessService::CatalogEntry> EMPTY_CATALOG;
} // namespace

bool BusinessService::validType(Type type)
{
    return type >= Type::Shop247 && type < Type::Count;
}

Vector3 BusinessService::backOf(const Vector3 &position, float angleDegrees, float distance)
{
    return Geometry::backOf(position, angleDegrees, distance);
}

// ------------------------------------------------------------------ реестр типов

void BusinessService::registerType(Type type, std::string name, std::string popupName,
                                   std::vector<CatalogEntry> catalog, std::vector<GoodDef> goods,
                                   VisitorMenu visitorMenu)
{
    if (!validType(type) || catalog.empty() || !visitorMenu || popupName.empty())
    {
        LogManager::log(Error, "BusinessService: тип бизнеса '" + name + "' зарегистрирован неполно, пропущен");
        return;
    }
    TypeDef &def = m_types[static_cast<std::size_t>(type)];
    def.registered = true;
    def.name = std::move(name);
    def.popupName = std::move(popupName);
    def.catalog = std::move(catalog);
    def.goods = std::move(goods);
    def.visitorMenu = std::move(visitorMenu);
    // def.services НЕ трогаем: услугу могли добавить до регистрации типа.
}

void BusinessService::addService(Type type, ServiceDef service)
{
    if (!validType(type) || service.name.empty() || service.popupName.empty() || service.price < 0 || !service.sell)
    {
        LogManager::log(Error, "BusinessService: услуга '" + service.name + "' зарегистрирована неполно, пропущена");
        return;
    }
    m_types[static_cast<std::size_t>(type)].services.push_back(std::move(service));
}

const std::vector<BusinessService::ServiceDef> &BusinessService::services(Type type) const
{
    static const std::vector<ServiceDef> EMPTY;
    // Не через typeRegistered: услуга могла приехать раньше регистрации типа.
    return validType(type) ? m_types[static_cast<std::size_t>(type)].services : EMPTY;
}

const std::vector<BusinessService::GoodDef> &BusinessService::goods(Type type) const
{
    static const std::vector<GoodDef> EMPTY;
    return typeRegistered(type) ? m_types[static_cast<std::size_t>(type)].goods : EMPTY;
}

// ------------------------------------------------------------------ склад точки

int BusinessService::stockCapOf(int businessId, int itemType) const
{
    const Business *business = getBusiness(businessId);
    if (!business)
    {
        return -1;
    }
    for (const GoodDef &good : goods(business->type))
    {
        if (good.itemType == itemType)
        {
            return good.stockCap;
        }
    }
    return -1; // товар не из ассортимента этого типа
}

int BusinessService::stockOf(int businessId, int itemType) const
{
    const Business *business = getBusiness(businessId);
    if (!business)
    {
        return 0;
    }
    const auto it = business->stock.find(itemType);
    return it == business->stock.end() ? 0 : it->second;
}

int BusinessService::stockRoom(int businessId, int itemType) const
{
    const int cap = stockCapOf(businessId, itemType);
    if (cap < 0)
    {
        return 0;
    }
    return std::max(0, cap - stockOf(businessId, itemType));
}

bool BusinessService::consumeStock(int businessId, int itemType, int count)
{
    if (count <= 0)
    {
        return false;
    }
    const auto it = m_businesses.find(businessId);
    if (it == m_businesses.end())
    {
        return false;
    }
    const auto slot = it->second.stock.find(itemType);
    if (slot == it->second.stock.end() || slot->second < count)
    {
        return false; // столько на складе нет — продажа не идёт
    }
    slot->second -= count;
    const int left = slot->second;
    for (const StockObserver &observer : m_stockObservers)
    {
        observer(businessId, itemType, left);
    }
    return true;
}

int BusinessService::addStock(int businessId, int itemType, int count)
{
    if (count <= 0)
    {
        return 0;
    }
    const auto it = m_businesses.find(businessId);
    if (it == m_businesses.end())
    {
        return 0;
    }
    // Потолок держим ЗДЕСЬ, а не у вызывающего: склад — состояние сервиса, и
    // переполнить его мимо проверки не должен ни один путь.
    const int cap = stockCapOf(businessId, itemType);
    if (cap < 0)
    {
        return 0;
    }
    int &quantity = it->second.stock[itemType];
    const int added = std::min(count, std::max(0, cap - quantity));
    if (added <= 0)
    {
        return 0;
    }
    quantity += added;
    const int now = quantity;
    for (const StockObserver &observer : m_stockObservers)
    {
        observer(businessId, itemType, now);
    }
    return added;
}

void BusinessService::loadStock(int businessId, int itemType, int quantity)
{
    const auto it = m_businesses.find(businessId);
    if (it == m_businesses.end() || quantity <= 0)
    {
        return; // осиротевшая строка либо пустой остаток — хранить нечего
    }
    const int cap = stockCapOf(businessId, itemType);
    if (cap < 0)
    {
        return; // товар выпал из ассортимента типа — остаток больше не наш
    }
    it->second.stock[itemType] = std::min(quantity, cap);
}

void BusinessService::subscribeStockChanged(StockObserver observer)
{
    if (observer)
    {
        m_stockObservers.push_back(std::move(observer));
    }
}

const std::string &BusinessService::typePopupName(Type type) const
{
    return typeRegistered(type) ? m_types[static_cast<std::size_t>(type)].popupName : EMPTY_NAME;
}

bool BusinessService::typeRegistered(Type type) const
{
    return validType(type) && m_types[static_cast<std::size_t>(type)].registered;
}

const std::string &BusinessService::typeName(Type type) const
{
    return typeRegistered(type) ? m_types[static_cast<std::size_t>(type)].name : EMPTY_NAME;
}

const std::vector<BusinessService::CatalogEntry> &BusinessService::catalog(Type type) const
{
    return typeRegistered(type) ? m_types[static_cast<std::size_t>(type)].catalog : EMPTY_CATALOG;
}

bool BusinessService::catalogValid(Type type, int interiorIndex) const
{
    return interiorIndex >= 0 && static_cast<std::size_t>(interiorIndex) < catalog(type).size();
}

bool BusinessService::openVisitorMenu(Type type, IPlayer &player, int businessId) const
{
    if (!typeRegistered(type))
    {
        return false;
    }
    m_types[static_cast<std::size_t>(type)].visitorMenu(player, businessId);
    return true;
}

std::vector<BusinessService::Type> BusinessService::registeredTypes() const
{
    std::vector<Type> types;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Type::Count); ++i)
    {
        const auto type = static_cast<Type>(i);
        if (typeRegistered(type))
        {
            types.push_back(type);
        }
    }
    return types;
}

// ------------------------------------------------------------------ запросы

const BusinessService::Business *BusinessService::getBusiness(int id) const
{
    const auto it = m_businesses.find(id);
    return it == m_businesses.end() ? nullptr : &it->second;
}

std::vector<int> BusinessService::businessesOf(const std::string &ownerKey) const
{
    std::vector<int> owned;
    if (ownerKey.empty())
    {
        return owned;
    }
    for (const auto &[id, business] : m_businesses)
    {
        if (business.owner == ownerKey)
        {
            owned.push_back(id);
        }
    }
    std::sort(owned.begin(), owned.end());
    return owned;
}

bool BusinessService::ownsBusiness(const std::string &ownerKey) const
{
    if (ownerKey.empty())
    {
        return false; // ничейный бизнес не «принадлежит» никому
    }
    for (const auto &[id, business] : m_businesses)
    {
        if (business.owner == ownerKey)
        {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ операции

const BusinessService::Business *BusinessService::createBusiness(Type type, const Vector3 &creatorPos,
                                                                 float creatorAngle, int interiorIndex,
                                                                 std::int64_t price)
{
    if (!typeRegistered(type) || !catalogValid(type, interiorIndex) || m_nextId > MAX_BUSINESS_ID)
    {
        return nullptr;
    }

    Business business;
    business.id = m_nextId++;
    business.type = type;
    business.interiorIndex = interiorIndex;
    business.entrance = creatorPos;
    // Угол ОКРУГЛЯЕМ до четверти оборота: сырой поворот игрока увёл бы точку выхода
    // с оси мира, и точка выглядела бы поставленной «под углом».
    business.exitAngle = Geometry::snapToQuarterTurn(creatorAngle);
    business.exit = backOf(creatorPos, business.exitAngle, EXIT_BEHIND);
    business.virtualWorld = VW_BASE + business.id;
    business.price = std::max<std::int64_t>(price, 0);

    const auto [it, inserted] = m_businesses.emplace(business.id, std::move(business));
    if (!inserted)
    {
        return nullptr;
    }
    notifyChanged();
    return &it->second;
}

bool BusinessService::removeBusiness(int id)
{
    if (m_businesses.erase(id) == 0)
    {
        return false;
    }
    notifyChanged();
    return true;
}

bool BusinessService::setPrice(int id, std::int64_t price)
{
    const auto it = m_businesses.find(id);
    if (it == m_businesses.end())
    {
        return false;
    }
    it->second.price = std::max<std::int64_t>(price, 0);
    notifyChanged();
    return true;
}

bool BusinessService::setOwner(int id, const std::string &ownerKey)
{
    const auto it = m_businesses.find(id);
    if (it == m_businesses.end())
    {
        return false;
    }
    const std::string oldKey = it->second.owner;
    it->second.owner = ownerKey;
    // json владение НЕ хранит (оно в БД) — notifyChanged здесь не нужен; персист
    // делает наблюдатель смены владельца.
    for (const OwnerChangedObserver &observer : m_ownerChangedObservers)
    {
        observer(id, oldKey, ownerKey);
    }
    return true;
}

bool BusinessService::setOwnerSilent(int id, const std::string &ownerKey)
{
    const auto it = m_businesses.find(id);
    if (it == m_businesses.end())
    {
        return false;
    }
    it->second.owner = ownerKey;
    return true;
}

void BusinessService::subscribeOwnerChanged(OwnerChangedObserver observer)
{
    if (observer)
    {
        m_ownerChangedObservers.push_back(std::move(observer));
    }
}

void BusinessService::markOwnershipLoaded()
{
    m_ownershipLoaded = true;
}

bool BusinessService::addIncome(int id, std::int64_t income)
{
    if (income <= 0)
    {
        return false;
    }
    const auto it = m_businesses.find(id);
    if (it == m_businesses.end())
    {
        return false;
    }
    it->second.balance += income;
    notifyChanged();
    // История выручки по дням — забота привода (он ходит в БД). Зовём ПОСЛЕ
    // зачисления: наблюдатель видит уже применённое состояние.
    for (const IncomeObserver &observer : m_incomeObservers)
    {
        observer(id, income);
    }
    return true;
}

void BusinessService::subscribeIncome(IncomeObserver observer)
{
    if (observer)
    {
        m_incomeObservers.push_back(std::move(observer));
    }
}

std::int64_t BusinessService::withdrawBalance(int id)
{
    const auto it = m_businesses.find(id);
    if (it == m_businesses.end() || it->second.balance <= 0)
    {
        return 0;
    }
    const std::int64_t amount = it->second.balance;
    it->second.balance = 0; // обнуляем СРАЗУ — анти-дюп двойного клика
    notifyChanged();
    return amount;
}

void BusinessService::subscribeChanged(ChangedObserver observer)
{
    if (observer)
    {
        m_changedObservers.push_back(std::move(observer));
    }
}

void BusinessService::notifyChanged() const
{
    for (const ChangedObserver &observer : m_changedObservers)
    {
        observer();
    }
}

// ------------------------------------------------------------------ json

std::string BusinessService::serialize() const
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto &[id, business] : m_businesses)
    {
        nlohmann::json item;
        item["id"] = business.id;
        item["type"] = static_cast<int>(business.type);
        item["interior"] = business.interiorIndex;
        item["entrance"] = {business.entrance.x, business.entrance.y, business.entrance.z};
        item["exit"] = {business.exit.x, business.exit.y, business.exit.z};
        item["exitAngle"] = business.exitAngle;
        item["vw"] = business.virtualWorld;
        item["price"] = business.price;
        item["balance"] = business.balance;
        // owner в json НЕ идёт: владение живёт в БД (business_owner), а Business::
        // owner — лишь зеркало. Иначе два источника правды разъехались бы.
        array.push_back(std::move(item));
    }

    // Объект с секцией, а не голый массив: формат остался от сборки, когда рядом
    // лежали торги (теперь они в auctions.json). Голый массив на загрузке тоже
    // читается — файл может быть от ещё более старой сборки.
    nlohmann::json root;
    root["businesses"] = std::move(array);
    return root.dump(2);
}

void BusinessService::loadBusiness(const Business &business)
{
    if (business.id <= 0 || business.id > MAX_BUSINESS_ID)
    {
        return;
    }
    Business normalized = business;
    // Тот же инвариант, что и при создании: угол — четверть оборота, точка выхода —
    // РОВНО EXIT_BEHIND за спиной от входа. Пересчитываем, а не берём из файла: иначе
    // точки, созданные до правки, остались бы со старым углом и старой дистанцией.
    normalized.exitAngle = Geometry::snapToQuarterTurn(business.exitAngle);
    normalized.exit = backOf(normalized.entrance, normalized.exitAngle, EXIT_BEHIND);
    m_businesses.emplace(normalized.id, std::move(normalized)); // дубликат id отбрасывается
}

void BusinessService::finalizeLoad()
{
    int maxId = 0;
    for (const auto &[id, business] : m_businesses)
    {
        maxId = std::max(maxId, id);
    }
    m_nextId = std::min(maxId + 1, MAX_BUSINESS_ID + 1);
}
