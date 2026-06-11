#include "Services/Core/PlayerSkinService/PlayerSkinService.h"

namespace
{
constexpr int MAX_SKIN = 311;
constexpr int MISSING_SKIN = 74; // единственный несуществующий id в SA
} // namespace

bool PlayerSkinService::isValidSkin(int skinId)
{
    return skinId >= 0 && skinId <= MAX_SKIN && skinId != MISSING_SKIN;
}

bool PlayerSkinService::setSkin(IPlayer &player, int skinId)
{
    if (!isValidSkin(skinId))
    {
        return false;
    }

    m_skins[player.getID()] = skinId;
    player.setSkin(skinId); // сразу, без ожидания респауна
    return true;
}

int PlayerSkinService::getSkin(int playerId) const
{
    return m_skins[playerId];
}

void PlayerSkinService::resetPlayer(int playerId)
{
    m_skins[playerId] = DEFAULT_SKIN;
}
