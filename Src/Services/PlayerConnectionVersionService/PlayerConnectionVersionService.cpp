#include "PlayerConnectionVersionService.h"

void PlayerConnectionVersionService::changeVersion(int playerID)
{
    m_playerConnectionVersions[playerID]++;
}

int PlayerConnectionVersionService::getVersion(int playerID) const
{
    return m_playerConnectionVersions[playerID];
}
