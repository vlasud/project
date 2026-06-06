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

void PlayerAuthService::setPlayerPassword(size_t playerId, const std::string &password)
{
    if (playerId >= m_authenticatedPlayers.size())
    {
        return;
    }
    m_playersPasswords[playerId] = password;
}

const std::string &PlayerAuthService::getPlayerPassword(size_t playerId) const
{
    static const std::string emptyPassword = "";

    if (playerId >= m_authenticatedPlayers.size())
    {
        return emptyPassword;
    }

    return m_playersPasswords[playerId];
}
