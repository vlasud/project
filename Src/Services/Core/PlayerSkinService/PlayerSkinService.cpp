#include "Services/Core/PlayerSkinService/PlayerSkinService.h"

namespace
{
constexpr int MIN_SKIN = 1;      // 0 (CJ) запрещён как применяемый скин
constexpr int MAX_SKIN = 311;
constexpr int MISSING_SKIN = 74; // единственный несуществующий id в SA
} // namespace

bool PlayerSkinService::isValidSkin(int skinId)
{
    // Валидны 1..311, кроме несуществующего 74. Скин 0 (CJ) запрещён: это
    // одновременно «сырое» дефолтное значение слота и сентинел «скин не задан»
    // (faction_member.skin == 0), поэтому как ПРИМЕНЯЕМЫЙ скин он недопустим.
    return skinId >= MIN_SKIN && skinId <= MAX_SKIN && skinId != MISSING_SKIN;
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
