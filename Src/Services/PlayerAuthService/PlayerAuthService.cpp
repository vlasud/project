#include "Services/PlayerAuthService/PlayerAuthService.h"

PlayerAuthService::EAuthState PlayerAuthService::getAuthState(size_t playerId) const
{
    return m_authState[playerId];
}

void PlayerAuthService::setPlayerAuthenticated(size_t playerId, EAuthState state)
{
    m_authState[playerId] = state;
}
