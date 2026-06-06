#include "PlayerConnectionVersionService.h"

void PlayerConnectionVersionService::changePlayerConnectionVersion(int playerID)
{
    m_playerConnectionVersions[playerID]++;
}

int PlayerConnectionVersionService::getPlayerConnectionVersion(int playerID) const
{
    return m_playerConnectionVersions[playerID];
}
