#include "Services/ReturnPointService/ReturnPointService.h"

#include "Utils/Sanitize.h"
#include <cmath>

bool ReturnPointService::isValid(const Point &point)
{
    const Vector3 &position = point.position;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return false;
    if (std::fabs(position.x) > WORLD_LIMIT || std::fabs(position.y) > WORLD_LIMIT ||
        std::fabs(position.z) > WORLD_LIMIT)
        return false;
    if (point.interior > static_cast<unsigned>(MAX_INTERIOR))
        return false;
    // Отрицательный виртуальный мир — правленая/битая строка: в таком мире нет ни
    // пикапов выхода, ни объектов, выбраться оттуда можно только смертью или релогом.
    return point.virtualWorld >= 0;
}

bool ReturnPointService::isLoaded(int playerId) const
{
    if (!validPlayerId(playerId))
        return false;
    return m_slots[playerId].loaded;
}

const ReturnPointService::Point *ReturnPointService::getPoint(int playerId) const
{
    if (!validPlayerId(playerId))
        return nullptr;
    const Slot &slot = m_slots[playerId];
    return slot.hasPoint ? &slot.point : nullptr;
}

bool ReturnPointService::isSpawned(int playerId) const
{
    if (!validPlayerId(playerId))
        return false;
    return m_slots[playerId].spawned;
}

void ReturnPointService::markSpawned(int playerId, TimePoint now)
{
    if (!validPlayerId(playerId))
        return;
    Slot &slot = m_slots[playerId];
    slot.spawned = true;
    slot.spawnedAt = now;
}

TimePoint ReturnPointService::spawnedAt(int playerId) const
{
    if (!validPlayerId(playerId))
        return {};
    return m_slots[playerId].spawnedAt;
}

bool ReturnPointService::isPromptDone(int playerId) const
{
    if (!validPlayerId(playerId))
        return false;
    return m_slots[playerId].promptDone;
}

void ReturnPointService::markPromptDone(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    m_slots[playerId].promptDone = true;
}

// ------------------------------------------------------------------ вызовы ReturnPointSystem

void ReturnPointService::load(int playerId, const std::optional<Point> &point)
{
    if (!validPlayerId(playerId))
        return;
    Slot &slot = m_slots[playerId];
    slot.loaded = true;
    slot.hasPoint = false;
    slot.point = Point{};
    if (!point)
        return;

    // Инвариант: в слоте лежит только годная точка — вызыватель телепортирует её
    // без собственных проверок.
    Point candidate = *point;
    candidate.angle = Utils::finiteOrZero(candidate.angle);
    if (!isValid(candidate))
        return;
    slot.hasPoint = true;
    slot.point = candidate;
}

void ReturnPointService::reset(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    m_slots[playerId] = Slot{};
}
