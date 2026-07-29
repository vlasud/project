#include "Services/BusinessService/BusinessService.h"

#include "Log/LogManager.h"
#include "Utils/Geometry/Geometry.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <utility>

namespace
{
// Точка выхода — за спиной создателя, чтобы выйдя он не стоял в пикапе входа.
constexpr float EXIT_BEHIND = 1.5f;

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

void BusinessService::registerType(Type type, std::string name, std::vector<CatalogEntry> catalog,
                                   VisitorMenu visitorMenu)
{
    if (!validType(type) || catalog.empty() || !visitorMenu)
    {
        LogManager::log(Error, "BusinessService: тип бизнеса '" + name + "' зарегистрирован неполно, пропущен");
        return;
    }
    TypeDef &def = m_types[static_cast<std::size_t>(type)];
    def.registered = true;
    def.name = std::move(name);
    def.catalog = std::move(catalog);
    def.visitorMenu = std::move(visitorMenu);
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
    business.exit = backOf(creatorPos, creatorAngle, EXIT_BEHIND);
    business.exitAngle = creatorAngle;
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
    return true;
}

bool BusinessService::addIncomeFrom(int id, std::int64_t income, const std::string &buyerKey)
{
    const Business *business = getBusiness(id);
    if (business && !buyerKey.empty() && business->owner == buyerKey)
    {
        // Покупает сам владелец: деньги с него уже сняты, но в копилку не кладём —
        // иначе он вернёт их себе через «Управление бизнесом» и получит товар даром.
        return false;
    }
    return addIncome(id, income);
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
    m_businesses.emplace(business.id, business); // дубликат id отбрасывается
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
