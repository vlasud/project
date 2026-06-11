#include "Services/Core/GangZoneService/GangZoneService.h"
#include "Log/LogManager.h"
#include "core.hpp"
#include <algorithm>
#include <cmath>

namespace
{
constexpr float WORLD_MIN = -3000.0f;
constexpr float WORLD_MAX = 3000.0f;
constexpr float MIN_PIECE_SIZE = 1.0f; // куски-щепки тоньше метра не создаём
constexpr std::size_t MAX_PIECES_PER_ZONE = 32;

float clampCoord(float value)
{
    return std::clamp(std::isfinite(value) ? value : 0.0f, WORLD_MIN, WORLD_MAX);
}

bool intersects(const GangZonePos &a, const GangZonePos &b)
{
    return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y;
}

GangZonePos makeRect(float x1, float y1, float x2, float y2)
{
    GangZonePos rect;
    rect.min = {x1, y1};
    rect.max = {x2, y2};
    return rect;
}

// a минус b — до четырёх прямоугольников (слева/справа во всю высоту,
// снизу/сверху между ними), добавляются в out.
void subtractRect(const GangZonePos &a, const GangZonePos &b, std::vector<GangZonePos> &out)
{
    if (!intersects(a, b))
    {
        out.push_back(a);
        return;
    }

    const float ix1 = std::max(a.min.x, b.min.x);
    const float iy1 = std::max(a.min.y, b.min.y);
    const float ix2 = std::min(a.max.x, b.max.x);
    const float iy2 = std::min(a.max.y, b.max.y);

    if (a.min.x < ix1)
    {
        out.push_back(makeRect(a.min.x, a.min.y, ix1, a.max.y));
    }
    if (ix2 < a.max.x)
    {
        out.push_back(makeRect(ix2, a.min.y, a.max.x, a.max.y));
    }
    if (a.min.y < iy1)
    {
        out.push_back(makeRect(ix1, a.min.y, ix2, iy1));
    }
    if (iy2 < a.max.y)
    {
        out.push_back(makeRect(ix1, iy2, ix2, a.max.y));
    }
}

bool isMeaningful(const GangZonePos &rect)
{
    return rect.max.x - rect.min.x >= MIN_PIECE_SIZE && rect.max.y - rect.min.y >= MIN_PIECE_SIZE;
}

// Полуоткрытые границы [min, max): общая грань двух соседних зон принадлежит
// ровно одной из них — двойного членства нет даже на стыке.
bool containsPoint(const GangZonePos &rect, Vector2 point)
{
    return point.x >= rect.min.x && point.x < rect.max.x && point.y >= rect.min.y && point.y < rect.max.y;
}
} // namespace

// ------------------------------------------------------------------ public

bool GangZoneService::isAvailable() const
{
    return m_gangZones != nullptr;
}

int GangZoneService::addZone(Vector2 cornerA, Vector2 cornerB, Colour colour)
{
    if (!m_gangZones)
    {
        LogManager::log(Error, "GangZoneService: IGangZonesComponent is missing");
        return -1;
    }

    Zone zone;
    zone.id = m_nextId++;
    zone.rect = normalizeRect(cornerA, cornerB);
    zone.colour = colour;
    m_zones.push_back(std::move(zone));

    rebuild();
    return m_zones.back().id;
}

void GangZoneService::removeZone(int zoneId)
{
    for (auto it = m_zones.begin(); it != m_zones.end(); ++it)
    {
        if (it->id == zoneId)
        {
            releasePieces(*it);
            m_zones.erase(it);
            rebuild(); // младшие зоны могли освободиться из-под обрезки
            return;
        }
    }
}

void GangZoneService::clearZones()
{
    for (Zone &zone : m_zones)
    {
        releasePieces(zone);
    }
    m_zones.clear();
}

bool GangZoneService::setZoneRect(int zoneId, Vector2 cornerA, Vector2 cornerB)
{
    Zone *zone = findZone(zoneId);
    if (!zone)
    {
        return false;
    }
    zone->rect = normalizeRect(cornerA, cornerB);
    rebuild();
    return true;
}

bool GangZoneService::setZoneColour(int zoneId, Colour colour)
{
    Zone *zone = findZone(zoneId);
    if (!zone)
    {
        return false;
    }
    zone->colour = colour;
    showZoneToAll(*zone); // геометрия не менялась — достаточно перепоказать
    return true;
}

bool GangZoneService::moveZonePriority(int zoneId, bool up)
{
    for (std::size_t i = 0; i < m_zones.size(); ++i)
    {
        if (m_zones[i].id != zoneId)
        {
            continue;
        }
        if (up && i > 0)
        {
            std::swap(m_zones[i], m_zones[i - 1]);
        }
        else if (!up && i + 1 < m_zones.size())
        {
            std::swap(m_zones[i], m_zones[i + 1]);
        }
        else
        {
            return false; // уже на краю
        }
        rebuild();
        return true;
    }
    return false;
}

bool GangZoneService::getZone(int zoneId, ZoneInfo &out) const
{
    const Zone *zone = findZone(zoneId);
    if (!zone)
    {
        return false;
    }
    out = ZoneInfo{zone->id, zone->rect, zone->colour, zone->pieces.size()};
    return true;
}

std::vector<GangZoneService::ZoneInfo> GangZoneService::listZones() const
{
    std::vector<ZoneInfo> result;
    result.reserve(m_zones.size());
    for (const Zone &zone : m_zones)
    {
        result.push_back(ZoneInfo{zone.id, zone.rect, zone.colour, zone.pieces.size()});
    }
    return result;
}

int GangZoneService::zoneAtPoint(Vector2 point) const
{
    // Эквивалентно проверке по обрезанным кускам: старшая зона, содержащая
    // точку, — единственная, у которой этот участок не вырезан.
    for (const Zone &zone : m_zones)
    {
        if (containsPoint(zone.rect, point))
        {
            return zone.id;
        }
    }
    return -1;
}

int GangZoneService::zoneAtPoint(const Vector3 &position) const
{
    return zoneAtPoint(Vector2(position.x, position.y));
}

bool GangZoneService::isPointInZone(int zoneId, Vector2 point) const
{
    return zoneAtPoint(point) == zoneId;
}

std::vector<GangZonePos> GangZoneService::getZonePieces(int zoneId) const
{
    std::vector<GangZonePos> result;
    const Zone *zone = findZone(zoneId);
    if (!zone)
    {
        return result;
    }
    result.reserve(zone->pieces.size());
    for (const IGangZone *piece : zone->pieces)
    {
        result.push_back(piece->getPosition());
    }
    return result;
}

void GangZoneService::flashZone(IPlayer &player, int zoneId, Colour flashColour, bool enable)
{
    Zone *zone = findZone(zoneId);
    if (!zone)
    {
        return;
    }
    for (IGangZone *piece : zone->pieces)
    {
        if (enable)
        {
            piece->flashForPlayer(player, flashColour);
        }
        else
        {
            piece->stopFlashForPlayer(player);
        }
    }
}

// ------------------------------------------------------------------ private

void GangZoneService::initialize(ICore *core, IGangZonesComponent *gangZones)
{
    m_core = core;
    m_gangZones = gangZones;
}

void GangZoneService::showAllForPlayer(IPlayer &player)
{
    for (const Zone &zone : m_zones)
    {
        for (IGangZone *piece : zone.pieces)
        {
            piece->showForPlayer(player, zone.colour);
        }
    }
}

void GangZoneService::showZoneToAll(const Zone &zone)
{
    if (!m_core)
    {
        return;
    }
    for (IPlayer *player : m_core->getPlayers().entries())
    {
        for (IGangZone *piece : zone.pieces)
        {
            piece->showForPlayer(*player, zone.colour);
        }
    }
}

void GangZoneService::rebuild()
{
    if (!m_gangZones)
    {
        return;
    }

    for (Zone &zone : m_zones)
    {
        releasePieces(zone);
    }

    std::vector<GangZonePos> higher; // прямоугольники уже обработанных (старших) зон
    std::vector<GangZonePos> pieces;
    std::vector<GangZonePos> next;

    for (Zone &zone : m_zones)
    {
        pieces.clear();
        pieces.push_back(zone.rect);

        for (const GangZonePos &clip : higher)
        {
            next.clear();
            for (const GangZonePos &piece : pieces)
            {
                subtractRect(piece, clip, next);
            }
            pieces.swap(next);
            if (pieces.size() > MAX_PIECES_PER_ZONE)
            {
                break;
            }
        }

        if (pieces.size() > MAX_PIECES_PER_ZONE)
        {
            pieces.resize(MAX_PIECES_PER_ZONE);
            LogManager::log(Warning, "GangZoneService: zone " + std::to_string(zone.id) +
                                         " clipped into too many pieces, truncated");
        }

        for (const GangZonePos &piece : pieces)
        {
            if (!isMeaningful(piece))
            {
                continue; // зона перекрыта почти целиком — щепки не рисуем
            }
            IGangZone *gangZone = m_gangZones->create(piece);
            if (!gangZone)
            {
                LogManager::log(Warning, "GangZoneService: gang zone pool is full");
                break;
            }
            zone.pieces.push_back(gangZone);
        }

        higher.push_back(zone.rect);
    }

    if (m_core)
    {
        for (IPlayer *player : m_core->getPlayers().entries())
        {
            showAllForPlayer(*player);
        }
    }
}

void GangZoneService::releasePieces(Zone &zone)
{
    for (IGangZone *piece : zone.pieces)
    {
        m_gangZones->release(piece->getID());
    }
    zone.pieces.clear();
}

GangZoneService::Zone *GangZoneService::findZone(int zoneId)
{
    for (Zone &zone : m_zones)
    {
        if (zone.id == zoneId)
        {
            return &zone;
        }
    }
    return nullptr;
}

const GangZoneService::Zone *GangZoneService::findZone(int zoneId) const
{
    for (const Zone &zone : m_zones)
    {
        if (zone.id == zoneId)
        {
            return &zone;
        }
    }
    return nullptr;
}

GangZonePos GangZoneService::normalizeRect(Vector2 a, Vector2 b)
{
    const float ax = clampCoord(a.x);
    const float ay = clampCoord(a.y);
    const float bx = clampCoord(b.x);
    const float by = clampCoord(b.y);
    return makeRect(std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by));
}
