#include "Services/InventoryService/InventoryService.h"

#include "Log/LogManager.h"
#include <fmt/format.h>
#include <utility>

void InventoryService::registerItem(int itemType, std::string name, int maxStack)
{
    if (itemType <= 0)
    {
        LogManager::log(Error, fmt::format("InventoryService: invalid itemType {} (must be > 0)", itemType));
        return;
    }
    if (maxStack < 1)
    {
        LogManager::log(Error, fmt::format("InventoryService: item {} has invalid maxStack {} (must be >= 1)", itemType,
                                           maxStack));
        return;
    }
    if (findDef(itemType) != nullptr)
    {
        LogManager::log(Error, fmt::format("InventoryService: duplicate itemType {} registration", itemType));
        return;
    }
    m_registry.push_back(ItemDef{itemType, std::move(name), maxStack});
}

const std::vector<InventoryService::ItemDef> &InventoryService::registeredItems() const
{
    return m_registry;
}

const InventoryService::ItemDef *InventoryService::findDef(int itemType) const
{
    for (const ItemDef &def : m_registry)
        if (def.itemType == itemType)
            return &def;
    return nullptr;
}

int InventoryService::maxStack(int itemType) const
{
    const ItemDef *def = findDef(itemType);
    return def ? def->maxStack : 0;
}

const std::string &InventoryService::itemName(int itemType) const
{
    static const std::string empty;
    for (const ItemDef &def : m_registry)
    {
        if (def.itemType == itemType)
        {
            return def.name;
        }
    }
    return empty;
}

int InventoryService::add(int playerId, int itemType, int n)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS || n <= 0)
        return 0;
    const ItemDef *def = findDef(itemType);
    if (!def)
        return 0; // незарегистрированный тип — выдать нечего

    PlayerItems &items = m_items[playerId];
    for (Stack &stack : items)
    {
        if (stack.itemType != itemType)
            continue;
        // Сколько влезает до потолка стека — добавляем не больше этого.
        const int room = def->maxStack - stack.count;
        if (room <= 0)
            return 0;
        const int added = n < room ? n : room;
        stack.count += added;
        return added;
    }
    // Стека ещё нет: создаём с зажимом к maxStack.
    const int added = n < def->maxStack ? n : def->maxStack;
    items.push_back(Stack{itemType, added});
    return added;
}

bool InventoryService::remove(int playerId, int itemType, int n)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS || n <= 0)
        return false;

    PlayerItems &items = m_items[playerId];
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        Stack &stack = items[i];
        if (stack.itemType != itemType)
            continue;
        if (stack.count < n)
            return false; // не хватает — атомарно НИЧЕГО не снимаем
        stack.count -= n;
        if (stack.count == 0)
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(i)); // пустой стек не держим
        return true;
    }
    return false; // нет стека этого типа
}

int InventoryService::count(int playerId, int itemType) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    for (const Stack &stack : m_items[playerId])
        if (stack.itemType == itemType)
            return stack.count;
    return 0;
}

void InventoryService::loadItems(int playerId, std::vector<std::pair<int, int>> typeQty)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;

    PlayerItems &items = m_items[playerId];
    items.clear();
    for (const auto &[itemType, qty] : typeQty)
    {
        const ItemDef *def = findDef(itemType);
        if (!def || qty <= 0)
            continue; // незарегистрированный тип или мусор — отбросить
        // Клампим к актуальному maxStack: устаревший избыток из БД не осядет.
        const int clamped = qty < def->maxStack ? qty : def->maxStack;
        items.push_back(Stack{itemType, clamped});
    }
}

std::vector<std::pair<int, int>> InventoryService::snapshot(int playerId) const
{
    std::vector<std::pair<int, int>> result;
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return result;
    for (const Stack &stack : m_items[playerId])
        if (stack.count > 0) // ненулевые не храним пустыми, но страхуемся
            result.emplace_back(stack.itemType, stack.count);
    return result;
}

void InventoryService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_items[playerId].clear();
}
