#include "Services/Core/MapIconService/MapIconService.h"

#include "Log/LogManager.h"
#include "Services/Core/StreamerService/StreamerService.h"
#include <algorithm>
#include <cmath>

namespace
{
float finiteOrZero(float value)
{
    return std::isfinite(value) ? value : 0.0f;
}
} // namespace

// ------------------------------------------------------------------ глобальные

int MapIconService::addGlobal(int iconType, const Vector3 &position, Colour colour, MapIconStyle style)
{
    if (!m_streamer)
    {
        LogManager::log(Error, "MapIconService: not initialized, icon dropped");
        return -1;
    }
    return m_streamer->addMapIcon(clampIconType(iconType), sanitizePosition(position), colour, clampStyle(style));
}

void MapIconService::removeGlobal(int iconId)
{
    if (m_streamer)
    {
        m_streamer->removeMapIcon(iconId);
    }
}

// ------------------------------------------------------------------ персональные

int MapIconService::setForPlayer(IPlayer &player, int iconType, const Vector3 &position, Colour colour,
                                 MapIconStyle style)
{
    auto &used = m_used[player.getID()];
    for (int i = 0; i < MANUAL_SLOTS; ++i)
    {
        if (used[i])
        {
            continue;
        }
        used[i] = true;
        const int slot = FIRST_MANUAL_SLOT + i;
        player.setMapIcon(slot, sanitizePosition(position), clampIconType(iconType), colour, clampStyle(style));
        return slot;
    }

    LogManager::log(Warning, "MapIconService: no free personal icon slots for player " +
                                 std::to_string(player.getID()));
    return -1;
}

bool MapIconService::updateForPlayer(IPlayer &player, int handle, int iconType, const Vector3 &position, Colour colour,
                                     MapIconStyle style)
{
    const int index = handle - FIRST_MANUAL_SLOT;
    if (index < 0 || index >= MANUAL_SLOTS || !m_used[player.getID()][index])
    {
        return false;
    }
    // Переустановка того же слота заменяет иконку на клиенте.
    player.setMapIcon(handle, sanitizePosition(position), clampIconType(iconType), colour, clampStyle(style));
    return true;
}

bool MapIconService::removeForPlayer(IPlayer &player, int handle)
{
    const int index = handle - FIRST_MANUAL_SLOT;
    if (index < 0 || index >= MANUAL_SLOTS || !m_used[player.getID()][index])
    {
        return false;
    }
    m_used[player.getID()][index] = false;
    player.unsetMapIcon(handle);
    return true;
}

void MapIconService::clearForPlayer(IPlayer &player)
{
    auto &used = m_used[player.getID()];
    for (int i = 0; i < MANUAL_SLOTS; ++i)
    {
        if (used[i])
        {
            used[i] = false;
            player.unsetMapIcon(FIRST_MANUAL_SLOT + i);
        }
    }
}

int MapIconService::personalCount(int playerId) const
{
    const auto &used = m_used[playerId];
    return static_cast<int>(std::count(used.begin(), used.end(), true));
}

// ------------------------------------------------------------------ private

void MapIconService::initialize(StreamerService *streamer)
{
    m_streamer = streamer;
}

void MapIconService::resetPlayer(int playerId)
{
    // Клиентское состояние умерло вместе с подключением — чистим только слоты.
    m_used[playerId].fill(false);
}

int MapIconService::clampIconType(int iconType)
{
    return std::clamp(iconType, 0, MAX_ICON_TYPE);
}

MapIconStyle MapIconService::clampStyle(MapIconStyle style)
{
    if (style < MapIconStyle_Local || style > MapIconStyle_GlobalCheckpoint)
    {
        return MapIconStyle_Local;
    }
    return style;
}

Vector3 MapIconService::sanitizePosition(Vector3 position)
{
    return {finiteOrZero(position.x), finiteOrZero(position.y), finiteOrZero(position.z)};
}
