#include "Services/Core/StreamerService/StreamerService.h"

#include <Server/Components/Objects/objects.hpp>
#include <Server/Components/TextLabels/textlabels.hpp>
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

// Гистерезис выхода: уже показанный def остаётся желанным, пока distSq не
// превысит streamDistSq * 1.44 (множитель 1.2 по дистанции, в квадрате) — иначе
// движение вдоль границы радиуса дёргало бы release/create каждый проход. Вход
// нового def'а — строго по streamDistSq. Обход сетки остаётся в радиусе
// MAX_STREAM_DISTANCE, поэтому у def'ов с радиусом около максимума гистерезис
// усечён им — это допустимо.
constexpr float EXIT_HYSTERESIS_SQ = 1.44f;

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
    m_candObjects.reserve(1024);
    m_candIcons.reserve(256);
    m_candLabels.reserve(256);
    m_desiredObjects.reserve(OBJECT_BUDGET);
    m_desiredIcons.reserve(ICON_BUDGET);
    m_desiredLabels.reserve(LABEL_BUDGET);
    m_scratchShown.reserve(OBJECT_BUDGET);
    m_pickupPoolToDef.reserve(256);
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

    // Немедленно убираем у всех, кому он сейчас показан (редкая операция). Идём по
    // онлайну (entries): у офлайн-слотов учёт очищен resetPlayer, снимать нечего.
    for (IPlayer *player : m_core->getPlayers().entries())
    {
        std::vector<Shown> &shown = m_players[player->getID()].objects;
        auto it = std::lower_bound(shown.begin(), shown.end(), defId,
                                   [](const Shown &s, int id) { return s.defId < id; });
        if (it == shown.end() || it->defId != defId)
            continue;

        if (IPlayerObjectData *objects = queryExtension<IPlayerObjectData>(*player))
            objects->release(it->clientId);
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
        m_pickupPoolToDef.erase(def.poolId); // индекс зеркалит только живые пул-экземпляры
        --m_activePickups;
    }
    def.used = false;
    def.poolId = -1;
    m_freePickupDefs.push_back(defId);
}

int StreamerService::pickupDefByPoolId(int poolId) const
{
    // O(1) по обратному индексу — резолв стоит на каждом pickup-RPC, а клиент
    // шлёт их повторно каждый кадр, пока игрок стоит на пикапе.
    const auto it = m_pickupPoolToDef.find(poolId);
    return it != m_pickupPoolToDef.end() ? it->second : -1;
}

bool StreamerService::getPickupInfo(int defId, Vector3 &position, std::uint32_t &virtualWorld) const
{
    if (defId < 0 || defId >= static_cast<int>(m_pickupDefs.size()) || !m_pickupDefs[defId].used)
    {
        return false;
    }
    position = m_pickupDefs[defId].position;
    virtualWorld = m_pickupDefs[defId].virtualWorld;
    return true;
}

int StreamerService::addTextLabel(StringView text, Colour colour, const Vector3 &position, float drawDistance,
                                  bool testLOS, float streamDistance)
{
    if (!m_grid)
        return -1;

    const int id = allocDef(m_labelDefs, m_freeLabelDefs);
    LabelDef &def = m_labelDefs[id];
    def = {};
    def.used = true;
    def.text = text.to_string();
    def.colour = colour;
    def.drawDistance = drawDistance;
    def.testLOS = testLOS;
    def.position = position;
    def.streamDistSq = clampStreamDistSq(streamDistance);
    def.gridHandle = m_grid->add(GridEntityType::TextLabel, id, position);
    return id;
}

void StreamerService::removeTextLabel(int defId)
{
    if (defId < 0 || defId >= static_cast<int>(m_labelDefs.size()) || !m_labelDefs[defId].used)
        return;

    LabelDef &def = m_labelDefs[defId];
    m_grid->remove(def.gridHandle);
    def.used = false;
    m_freeLabelDefs.push_back(defId);

    for (IPlayer *player : m_core->getPlayers().entries())
    {
        std::vector<Shown> &shown = m_players[player->getID()].labels;
        auto it = std::lower_bound(shown.begin(), shown.end(), defId,
                                   [](const Shown &s, int id) { return s.defId < id; });
        if (it == shown.end() || it->defId != defId)
            continue;

        if (IPlayerTextLabelData *labels = queryExtension<IPlayerTextLabelData>(*player))
            labels->release(it->clientId);
        shown.erase(it);
    }
}

bool StreamerService::updateTextLabel(int defId, StringView text, Colour colour)
{
    if (defId < 0 || defId >= static_cast<int>(m_labelDefs.size()) || !m_labelDefs[defId].used)
        return false;

    LabelDef &def = m_labelDefs[defId];
    def.text = text.to_string();
    def.colour = colour;

    // Живое обновление тем, кому показан: setColourAndText делает restream —
    // hide+show, 2 RPC на игрока. Идём по онлайну (entries): у офлайн-слотов
    // учёт очищен resetPlayer.
    for (IPlayer *player : m_core->getPlayers().entries())
    {
        std::vector<Shown> &shown = m_players[player->getID()].labels;
        auto it = std::lower_bound(shown.begin(), shown.end(), defId,
                                   [](const Shown &s, int id) { return s.defId < id; });
        if (it == shown.end() || it->defId != defId)
            continue;

        if (IPlayerTextLabelData *labels = queryExtension<IPlayerTextLabelData>(*player))
        {
            if (IPlayerTextLabel *label = labels->get(it->clientId))
                label->setColourAndText(def.colour, def.text);
        }
    }
    return true;
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

    for (IPlayer *player : m_core->getPlayers().entries())
    {
        PerPlayer &pp = m_players[player->getID()];
        auto it = std::lower_bound(pp.icons.begin(), pp.icons.end(), defId,
                                   [](const Shown &s, int id) { return s.defId < id; });
        if (it == pp.icons.end() || it->defId != defId)
            continue;

        player->unsetMapIcon(it->clientId);
        pp.freeIconSlots.push_back(it->clientId);
        pp.icons.erase(it);
    }
}

// --- стриминг ---

bool StreamerService::isShown(const std::vector<Shown> &shown, int defId)
{
    const auto it = std::lower_bound(shown.begin(), shown.end(), defId,
                                     [](const Shown &s, int id) { return s.defId < id; });
    return it != shown.end() && it->defId == defId;
}

void StreamerService::selectDesired(std::vector<Candidate> &candidates, int budget, std::vector<int> &desired)
{
    // Бюджет достаётся ближним. Полная сортировка не нужна: при переполнении
    // отделяем budget ближайших за O(n) (nth_element), в пределах бюджета
    // порядок не важен — diff* сортируют desired по defId. Отбор чисто по
    // дистанции, поэтому дальний показанный (гистерезис) не вытеснит ближнего
    // нового. Усечение in-place, без аллокаций.
    if (static_cast<int>(candidates.size()) > budget)
    {
        std::nth_element(candidates.begin(), candidates.begin() + budget, candidates.end(),
                         [](const Candidate &a, const Candidate &b) { return a.distSq < b.distSq; });
        candidates.resize(budget);
    }
    desired.clear();
    for (const Candidate &candidate : candidates)
        desired.push_back(candidate.defId);
}

void StreamerService::streamPlayer(IPlayer &player, const Vector3 &position, int virtualWorld, TimePoint now)
{
    PerPlayer &pp = m_players[player.getID()];
    if (now < pp.nextStreamAt)
        return;
    pp.nextStreamAt = now + STREAM_INTERVAL;

    m_candObjects.clear();
    m_candIcons.clear();
    m_candLabels.clear();
    const std::uint32_t vw = static_cast<std::uint32_t>(virtualWorld);

    // Один обход сетки по всем стримящимся типам, фильтр по радиусу стрима
    // def'а на месте — до всяких сортировок. Показанному def'у даём гистерезис
    // выхода (isShown зовётся только в кольце между радиусом и его 1.2 — O(log S)
    // на кандидата в кольце), вход нового — строго по радиусу. Пикапы в
    // кандидаты не кладём: им не нужен порядок, проход только помечает «нужен»,
    // и только в мире игрока — иначе один игрок в интерьере оживлял бы
    // одноточечные пикапы всех миров, раздувая активный пул, который open.mp
    // сканирует целиком на стрим-тик каждого игрока.
    m_grid->forEachInRadius(
        position, MAX_STREAM_DISTANCE,
        gridMask(GridEntityType::Object) | gridMask(GridEntityType::Pickup) | gridMask(GridEntityType::MapIcon) |
            gridMask(GridEntityType::TextLabel),
        [&](GridEntityType type, std::int32_t id, float distSq) {
            switch (type)
            {
            case GridEntityType::Object:
            {
                const float limit = m_objectDefs[id].streamDistSq;
                if (distSq <= limit || (distSq <= limit * EXIT_HYSTERESIS_SQ && isShown(pp.objects, id)))
                    m_candObjects.push_back({id, distSq});
                break;
            }
            case GridEntityType::MapIcon:
            {
                const float limit = m_iconDefs[id].streamDistSq;
                if (distSq <= limit || (distSq <= limit * EXIT_HYSTERESIS_SQ && isShown(pp.icons, id)))
                    m_candIcons.push_back({id, distSq});
                break;
            }
            case GridEntityType::TextLabel:
            {
                const float limit = m_labelDefs[id].streamDistSq;
                if (distSq <= limit || (distSq <= limit * EXIT_HYSTERESIS_SQ && isShown(pp.labels, id)))
                    m_candLabels.push_back({id, distSq});
                break;
            }
            case GridEntityType::Pickup:
            {
                PickupDef &def = m_pickupDefs[id];
                if (def.virtualWorld == vw && distSq <= def.streamDistSq)
                    def.lastWanted = now;
                break;
            }
            default:
                break;
            }
        });

    selectDesired(m_candObjects, OBJECT_BUDGET, m_desiredObjects);
    selectDesired(m_candIcons, ICON_BUDGET, m_desiredIcons);
    selectDesired(m_candLabels, LABEL_BUDGET, m_desiredLabels);

    diffObjects(player, pp);
    diffIcons(player, pp);
    diffLabels(player, pp);
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

    // Две фазы: merge-diff сначала выполняет ВСЕ release (слоты возвращаются в
    // freeIconSlots), а create-кандидатов компактит в начало m_desiredIcons
    // (запись отстаёт от чтения — in-place безопасно). Иначе при полном бюджете
    // create упирался бы в пустой пул слотов раньше release — телепорт «моргал»
    // бы иконками на целый проход.
    std::size_t si = 0;
    std::size_t di = 0;
    std::size_t pending = 0; // create-кандидатов скопировано в начало m_desiredIcons
    while (si < pp.icons.size() || di < m_desiredIcons.size())
    {
        if (di == m_desiredIcons.size() || (si < pp.icons.size() && pp.icons[si].defId < m_desiredIcons[di]))
        {
            player.unsetMapIcon(pp.icons[si].clientId); // вышел из зоны — убрать
            pp.freeIconSlots.push_back(pp.icons[si].clientId);
            ++si;
        }
        else if (si == pp.icons.size() || m_desiredIcons[di] < pp.icons[si].defId)
        {
            m_desiredIcons[pending++] = m_desiredIcons[di]; // вошёл — создать во второй фазе
            ++di;
        }
        else
        {
            m_scratchShown.push_back(pp.icons[si]); // уже показан
            ++si;
            ++di;
        }
    }

    // Фаза create: kept + pending <= ICON_BUDGET, после release слотов хватает
    // всегда (гард — страховка инварианта).
    for (std::size_t i = 0; i < pending && !pp.freeIconSlots.empty(); ++i)
    {
        const int slot = pp.freeIconSlots.back();
        pp.freeIconSlots.pop_back();
        const IconDef &def = m_iconDefs[m_desiredIcons[i]];
        player.setMapIcon(slot, def.position, def.iconType, def.colour, def.style);
        m_scratchShown.push_back({m_desiredIcons[i], slot});
    }

    // Созданные дописаны после оставшихся — восстановить сортировку по defId
    // (инвариант pp.icons для merge-diff и бинарных поисков). <=90 элементов,
    // сортировка in-place.
    std::sort(m_scratchShown.begin(), m_scratchShown.end(),
              [](const Shown &a, const Shown &b) { return a.defId < b.defId; });
    pp.icons.swap(m_scratchShown);
}

void StreamerService::diffLabels(IPlayer &player, PerPlayer &pp)
{
    IPlayerTextLabelData *labels = queryExtension<IPlayerTextLabelData>(player);
    if (!labels)
        return;

    std::sort(m_desiredLabels.begin(), m_desiredLabels.end());
    m_scratchShown.clear();

    std::size_t si = 0;
    std::size_t di = 0;
    while (si < pp.labels.size() || di < m_desiredLabels.size())
    {
        if (di == m_desiredLabels.size() || (si < pp.labels.size() && pp.labels[si].defId < m_desiredLabels[di]))
        {
            labels->release(pp.labels[si].clientId); // вышел из зоны — убрать
            ++si;
        }
        else if (si == pp.labels.size() || m_desiredLabels[di] < pp.labels[si].defId)
        {
            const LabelDef &def = m_labelDefs[m_desiredLabels[di]]; // вошёл — создать
            IPlayerTextLabel *label = labels->create(def.text, def.colour, def.position, def.drawDistance, def.testLOS);
            if (label)
                m_scratchShown.push_back({m_desiredLabels[di], label->getID()});
            ++di;
        }
        else
        {
            m_scratchShown.push_back(pp.labels[si]); // уже показан
            ++si;
            ++di;
        }
    }
    pp.labels.swap(m_scratchShown);
}

void StreamerService::sweepPickups(TimePoint now)
{
    if (!m_pickups)
        return;

    // O(defs) раз в секунду; обратный индекс правится только на переходах пула
    // (create/release), не на каждом проходе.
    for (std::size_t defId = 0; defId < m_pickupDefs.size(); ++defId)
    {
        PickupDef &def = m_pickupDefs[defId];
        if (!def.used)
            continue;

        const bool wanted = (now - def.lastWanted) <= PICKUP_KEEP;
        if (wanted && def.poolId < 0)
        {
            IPickup *pickup = m_pickups->create(def.model, def.type, def.position, def.virtualWorld, false);
            def.poolId = pickup ? pickup->getID() : -1;
            if (def.poolId >= 0)
            {
                // Ядро переиспользует poolId после release — мы стираем запись
                // при release, поэтому вставка всегда свежая ([] перезапишет и
                // при рассинхроне, не оставив висячего defId).
                m_pickupPoolToDef[def.poolId] = static_cast<int>(defId);
                ++m_activePickups;
            }
        }
        else if (!wanted && def.poolId >= 0)
        {
            m_pickups->release(def.poolId);
            m_pickupPoolToDef.erase(def.poolId);
            def.poolId = -1;
            --m_activePickups;
        }
    }
}

void StreamerService::resetPlayer(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    // Клиентские сущности умирают вместе с соединением — чистим только учёт.
    m_players[playerId] = PerPlayer{};
}
