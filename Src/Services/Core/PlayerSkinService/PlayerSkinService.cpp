#include "Services/Core/PlayerSkinService/PlayerSkinService.h"

#include <utility>

namespace
{
constexpr int MIN_SKIN = 1;      // 0 (CJ) запрещён как применяемый скин
constexpr int MAX_SKIN = 311;
constexpr int MISSING_SKIN = 74; // единственный несуществующий id в SA
constexpr int NO_TEMP = -1;      // временного оверрайда нет
} // namespace

PlayerSkinService::PlayerSkinService()
{
    m_baseSkin.fill(DEFAULT_SKIN);
    m_tempSkin.fill(NO_TEMP);
}

bool PlayerSkinService::isValidSkin(int skinId)
{
    // Валидны 1..311, кроме несуществующего 74. Скин 0 (CJ) запрещён: это
    // одновременно «сырое» дефолтное значение слота и сентинел «скин не задан»
    // (faction_member.skin == 0), поэтому как ПРИМЕНЯЕМЫЙ скин он недопустим.
    return skinId >= MIN_SKIN && skinId <= MAX_SKIN && skinId != MISSING_SKIN;
}

bool PlayerSkinService::isValidId(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}

bool PlayerSkinService::setSkin(IPlayer &player, int skinId)
{
    const int id = player.getID();
    if (!isValidId(id) || !isValidSkin(skinId))
    {
        return false;
    }

    m_baseSkin[id] = skinId;
    // Если активен temp — на экране остаётся он; база покажется после респауна
    // (clearTempSkin). Иначе применяем базу сразу, без ожидания респауна.
    if (m_tempSkin[id] == NO_TEMP)
    {
        player.setSkin(skinId);
    }
    // База сменилась — даём PlayerSpawnService пересобрать спавн-инфо, чтобы
    // нативный респаун после смерти не откатил игрока на прежнюю базу. Зовём и при
    // активном temp: спавн-инфо хранит именно базу (temp на спавне сбрасывается).
    for (const BaseSkinObserver &observer : m_baseObservers)
    {
        observer(player);
    }
    return true;
}

bool PlayerSkinService::setTempSkin(IPlayer &player, int skinId)
{
    const int id = player.getID();
    if (!isValidId(id) || !isValidSkin(skinId))
    {
        return false;
    }

    m_tempSkin[id] = skinId;
    player.setSkin(skinId); // временный показываем немедленно
    return true;
}

void PlayerSkinService::clearTempSkin(IPlayer &player)
{
    const int id = player.getID();
    if (!isValidId(id) || m_tempSkin[id] == NO_TEMP)
    {
        return;
    }

    m_tempSkin[id] = NO_TEMP;
    player.setSkin(m_baseSkin[id]); // вернуть базу на клиент
}

bool PlayerSkinService::hasTempSkin(int playerId) const
{
    return isValidId(playerId) && m_tempSkin[playerId] != NO_TEMP;
}

int PlayerSkinService::getSkin(int playerId) const
{
    return isValidId(playerId) ? m_baseSkin[playerId] : DEFAULT_SKIN;
}

void PlayerSkinService::subscribeBaseChange(BaseSkinObserver observer)
{
    m_baseObservers.push_back(std::move(observer));
}

void PlayerSkinService::resetPlayer(int playerId)
{
    if (!isValidId(playerId))
    {
        return;
    }
    m_baseSkin[playerId] = DEFAULT_SKIN;
    m_tempSkin[playerId] = NO_TEMP;
}
