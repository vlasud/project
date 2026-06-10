#include "Services/Core/StreamerService/StreamerService.h"

#include <Server/Components/Objects/objects.hpp>
#include <algorithm>
#include <chrono>

namespace
{
// Период пересчёта видимости для игрока. Игроки троттлятся каждый своим
// дедлайном, поэтому нагрузка размазана по тикам, а не приходит пиками.
constexpr std::chrono::milliseconds STREAM_INTERVAL{500};

// Пикап живёт в пуле ещё столько после того, как последний игрок ушёл из его
// радиуса (с запасом больше STREAM_INTERVAL, чтобы не мигал на границе).
constexpr std::chrono::milliseconds PICKUP_KEEP{2000};

// Бюджеты на игрока. Клиентский лимит объектов ~1000 (включая глобальные),
// иконок — 100; берём с запасом под ручное использование.
constexpr int OBJECT_BUDGET = 400;
constexpr int ICON_BUDGET = 90; // слоты 0..89, слоты 90..99 свободны для ручных иконок

template <typename Def> int allocDef(std::vector<Def> &defs, std::vector<int> &freeList)
{
    if (!freeList.empty())
    {
        const int id = freeList.back();
        freeList.pop_back();
        return id;
    }
    defs.emplace_back();
    return static_cast<int>(defs.size()) - 1;
}

float clampStreamDistSq(float distance)
{
    if (distance > StreamerService::MAX_STREAM_DISTANCE)
        distance = StreamerService::MAX_STREAM_DISTANCE;
    return distance * distance;
}
} // namespace

void StreamerService::initialize(ICore &core, GridService &grid, IPickupsComponent *pickups)
{
    m_core = &core;
    m_grid = &grid;
    m_pickups = pickups;
    m_candidates.reserve(1024);
    m_desiredObjects.reserve(OBJECT_BUDGET);
    m_desiredIcons.reserve(ICON_BUDGET);
    m_scratchShown.reserve(OBJECT_BUDGET);
}

// --- контент ---

int StreamerService::addObject(int model, const Vector3 &position, const Vector3 &rotation, float streamDistance,
                               float drawDistance)
{
    if (!m_grid)
        return -1;

    const int id = allocDef(m_objectDefs, m_freeObjectDefs);
    ObjectDef &def = m_objectDefs[id];
    def = {};
    def.used = true;
    def.model = model;
    def.position = position;
    def.rotation = rotation;
    def.drawDistance = drawDistance;
    def.streamDistSq = clampStreamDistSq(streamDistance);
    def.gridHandle = m_grid->add(GridEntityType::Object, id, position);
    return id;
}

void StreamerService::removeObject(int defId)
{
    if (defId < 0 || defId >= static_cast<int>(m_objectDefs.size()) || !m_objectDefs[defId].used)
        return;

    ObjectDef &def = m_objectDefs[defId];
    m_grid->remove(def.gridHandle);
    def.used = false;
    m_freeObjectDefs.push_back(defId);

    // Немедленно убираем у всех, кому он сейчас показан (редкая операция).
    for (int playerId = 0; playerId < MAX_PLAYERS; ++playerId)
    {
        std::vector<Shown> &shown = m_players[playerId].objects;
        auto it = std::lower_bound(shown.begin(), shown.end(), defId,
                                   [](const Shown &s, int id) { return s.defId < id; });
        if (it == shown.end() || it->defId != defId)
            continue;

        IPlayer *player = m_core->getPlayers().get(playerId);
        if (player)
        {
            if (IPlayerObjectData *objects = queryExtension<IPlayerObjectData>(*player))
                objects->release(it->clientId);
        }
        shown.erase(it);
    }
}

int StreamerService::addPickup(int model, PickupType type, const Vector3 &position, std::uint32_t virtualWorld,
                               float streamDistance)
{
    if (!m_grid)
        return -1;

    const int id = allocDef(m_pickupDefs, m_freePickupDefs);
    PickupDef &def = m_pickupDefs[id];
    def = {};
    def.used = true;
    def.model = model;
    def.type = type;
    def.virtualWorld = virtualWorld;
    def.position = position;
    def.streamDistSq = clampStreamDistSq(streamDistance);
    def.gridHandle = m_grid->add(GridEntityType::Pickup, id, position);
    return id;
}

void StreamerService::removePickup(int defId)
{
    if (defId < 0 || defId >= static_cast<int>(m_pickupDefs.size()) || !m_pickupDefs[defId].used)
        return;

    PickupDef &def = m_pickupDefs[defId];
    m_grid->remove(def.gridHandle);
    if (def.poolId >= 0 && m_pickups)
    {
        m_pickups->release(def.poolId);
        --m_activePickups;
    }
    def.used = false;
    def.poolId = -1;
    m_freePickupDefs.push_back(defId);
}

int StreamerService::addMapIcon(int iconType, const Vector3 &position, Colour colour, MapIconStyle style,
                                float streamDistance)
{
    if (!m_grid)
        return -1;

    const int id = allocDef(m_iconDefs, m_freeIconDefs);
    IconDef &def = m_iconDefs[id];
    def = {};
    def.used = true;
    def.iconType = iconType;
    def.colour = colour;
    def.style = style;
    def.position = position;
    def.streamDistSq = clampStreamDistSq(streamDistance);
    def.gridHandle = m_grid->add(GridEntityType::MapIcon, id, position);
    return id;
}

void StreamerService::removeMapIcon(int defId)
{
    if (defId < 0 || defId >= static_cast<int>(m_iconDefs.size()) || !m_iconDefs[defId].used)
        return;

    IconDef &def = m_iconDefs[defId];
    m_grid->remove(def.gridHandle);
    def.used = false;
    m_freeIconDefs.push_back(defId);

    for (int playerId = 0; playerId < MAX_PLAYERS; ++playerId)
    {
        PerPlayer &pp = m_players[playerId];
        auto it = std::lower_bound(pp.icons.begin(), pp.icons.end(), defId,
                                   [](const Shown &s, int id) { return s.defId < id; });
        if (it == pp.icons.end() || it->defId != defId)
            continue;

        if (IPlayer *player = m_core->getPlayers().get(playerId))
            player->unsetMapIcon(it->clientId);
        pp.freeIconSlots.push_back(it->clientId);
        pp.icons.erase(it);
    }
}

// --- стриминг ---

void StreamerService::streamPlayer(IPlayer &player, const Vector3 &position, TimePoint now)
{
    PerPlayer &pp = m_players[player.getID()];
    if (now < pp.nextStreamAt)
        return;
    pp.nextStreamAt = now + STREAM_INTERVAL;

    // Кандидаты из сетки одним запросом по всем стримящимся типам.
    const Vector3 &pos = position;
    m_grid->queryRadius(pos, MAX_STREAM_DISTANCE,
                        gridMask(GridEntityType::Object) | gridMask(GridEntityType::Pickup) |
                            gridMask(GridEntityType::MapIcon),
                        m_candidates);

    // Ближние первыми — бюджеты достаются ближайшим.
    std::sort(m_candidates.begin(), m_candidates.end(),
              [](const GridService::Result &a, const GridService::Result &b) { return a.distSq < b.distSq; });

    m_desiredObjects.clear();
    m_desiredIcons.clear();
    int objectBudget = OBJECT_BUDGET;
    int iconBudget = ICON_BUDGET;

    for (const GridService::Result &candidate : m_candidates)
    {
        switch (candidate.type)
        {
        case GridEntityType::Object:
            if (objectBudget > 0 && candidate.distSq <= m_objectDefs[candidate.id].streamDistSq)
            {
                m_desiredObjects.push_back(candidate.id);
                --objectBudget;
            }
            break;
        case GridEntityType::MapIcon:
            if (iconBudget > 0 && candidate.distSq <= m_iconDefs[candidate.id].streamDistSq)
            {
                m_desiredIcons.push_back(candidate.id);
                --iconBudget;
            }
            break;
        case GridEntityType::Pickup:
            // Пикапы глобальные: проход только помечает «нужен», создаёт развёртка.
            if (candidate.distSq <= m_pickupDefs[candidate.id].streamDistSq)
                m_pickupDefs[candidate.id].lastWanted = now;
            break;
        default:
            break;
        }
    }

    diffObjects(player, pp);
    diffIcons(player, pp);
}

void StreamerService::diffObjects(IPlayer &player, PerPlayer &pp)
{
    IPlayerObjectData *objects = queryExtension<IPlayerObjectData>(player);
    if (!objects)
        return;

    // Merge-diff двух массивов, отсортированных по defId: O(shown + desired),
    // создаём/удаляем только фактическую разницу.
    std::sort(m_desiredObjects.begin(), m_desiredObjects.end());
    m_scratchShown.clear();

    std::size_t si = 0;
    std::size_t di = 0;
    while (si < pp.objects.size() || di < m_desiredObjects.size())
    {
        if (di == m_desiredObjects.size() ||
            (si < pp.objects.size() && pp.objects[si].defId < m_desiredObjects[di]))
        {
            objects->release(pp.objects[si].clientId); // вышел из зоны — убрать
            ++si;
        }
        else if (si == pp.objects.size() || m_desiredObjects[di] < pp.objects[si].defId)
        {
            const ObjectDef &def = m_objectDefs[m_desiredObjects[di]]; // вошёл — создать
            IPlayerObject *object = objects->create(def.model, def.position, def.rotation, def.drawDistance);
            if (object)
                m_scratchShown.push_back({m_desiredObjects[di], object->getID()});
            ++di;
        }
        else
        {
            m_scratchShown.push_back(pp.objects[si]); // уже показан
            ++si;
            ++di;
        }
    }
    pp.objects.swap(m_scratchShown);
}

void StreamerService::diffIcons(IPlayer &player, PerPlayer &pp)
{
    if (!pp.iconSlotsInit)
    {
        pp.iconSlotsInit = true;
        pp.freeIconSlots.reserve(ICON_BUDGET);
        for (int slot = ICON_BUDGET - 1; slot >= 0; --slot)
            pp.freeIconSlots.push_back(slot);
    }

    std::sort(m_desiredIcons.begin(), m_desiredIcons.end());
    m_scratchShown.clear();

    std::size_t si = 0;
    std::size_t di = 0;
    while (si < pp.icons.size() || di < m_desiredIcons.size())
    {
        if (di == m_desiredIcons.size() || (si < pp.icons.size() && pp.icons[si].defId < m_desiredIcons[di]))
        {
            player.unsetMapIcon(pp.icons[si].clientId);
            pp.freeIconSlots.push_back(pp.icons[si].clientId);
            ++si;
        }
        else if (si == pp.icons.size() || m_desiredIcons[di] < pp.icons[si].defId)
        {
            if (!pp.freeIconSlots.empty())
            {
                const int slot = pp.freeIconSlots.back();
                pp.freeIconSlots.pop_back();
                const IconDef &def = m_iconDefs[m_desiredIcons[di]];
                player.setMapIcon(slot, def.position, def.iconType, def.colour, def.style);
                m_scratchShown.push_back({m_desiredIcons[di], slot});
            }
            ++di;
        }
        else
        {
            m_scratchShown.push_back(pp.icons[si]);
            ++si;
            ++di;
        }
    }
    pp.icons.swap(m_scratchShown);
}

void StreamerService::sweepPickups(TimePoint now)
{
    if (!m_pickups)
        return;

    for (PickupDef &def : m_pickupDefs)
    {
        if (!def.used)
            continue;

        const bool wanted = (now - def.lastWanted) <= PICKUP_KEEP;
        if (wanted && def.poolId < 0)
        {
            IPickup *pickup = m_pickups->create(def.model, def.type, def.position, def.virtualWorld, false);
            def.poolId = pickup ? pickup->getID() : -1;
            if (def.poolId >= 0)
                ++m_activePickups;
        }
        else if (!wanted && def.poolId >= 0)
        {
            m_pickups->release(def.poolId);
            def.poolId = -1;
            --m_activePickups;
        }
    }
}

void StreamerService::resetPlayer(int playerId)
{
    // Клиентские сущности умирают вместе с соединением — чистим только учёт.
    m_players[playerId] = PerPlayer{};
}
