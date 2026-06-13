#include "Services/PlayerPersonalSkinService/PlayerPersonalSkinService.h"

#include "Services/Core/PlayerSkinService/PlayerSkinService.h"

namespace
{
constexpr int NOT_SEEDED = -1;
} // namespace

PlayerPersonalSkinService::PlayerPersonalSkinService()
{
    m_personalSkin.fill(NOT_SEEDED);
}

int PlayerPersonalSkinService::defaultSkinForSex(std::uint8_t sex)
{
    // ESex: 0 — мужской, 1 — женский.
    return sex == 0 ? DEFAULT_SKIN_MALE : DEFAULT_SKIN_FEMALE;
}

bool PlayerPersonalSkinService::setSkin(int playerId, int skinId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    // Валидация против тех же правил, что и применённый скин: мусор из БД или
    // от будущего сменщика не должен осесть как «личный».
    if (!PlayerSkinService::isValidSkin(skinId))
        return false;

    m_personalSkin[playerId] = skinId;
    return true;
}

int PlayerPersonalSkinService::getSkin(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return DEFAULT_SKIN_MALE;
    const int skin = m_personalSkin[playerId];
    return skin == NOT_SEEDED ? DEFAULT_SKIN_MALE : skin;
}

void PlayerPersonalSkinService::resetPlayer(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_personalSkin[playerId] = NOT_SEEDED;
}
