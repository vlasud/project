#include "PlayerAuthService.h"

bool PlayerAuthService::isPlayerAuthenticated(size_t playerId) const
{
    if (playerId >= m_authenticatedPlayers.size())
    {
        return false;
    }
    return m_authenticatedPlayers[playerId];
}

void PlayerAuthService::setPlayerAuthenticated(size_t playerId, bool authenticated)
{
    if (playerId >= m_authenticatedPlayers.size())
    {
        return;
    }
    m_authenticatedPlayers[playerId] = authenticated;
}
