#include "Services/Core/PlayerSavedLocationService/PlayerSavedLocationService.h"

void PlayerSavedLocationService::save(int playerId, const Vector3 &position, unsigned interior, int virtualWorld)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_saved[playerId] = {position, interior, virtualWorld, true};
}

const PlayerSavedLocationService::Saved *PlayerSavedLocationService::get(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return nullptr;
    const Saved &saved = m_saved[playerId];
    return saved.valid ? &saved : nullptr;
}

void PlayerSavedLocationService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_saved[playerId] = Saved{};
}
