#include "Services/PlayerWeaponPersistService/PlayerWeaponPersistService.h"

bool PlayerWeaponPersistService::areWeaponsLoaded(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    return m_weapons[playerId].loaded;
}

const std::vector<std::pair<std::uint8_t, int>> &PlayerWeaponPersistService::cachedWeapons(int playerId) const
{
    static const std::vector<std::pair<std::uint8_t, int>> EMPTY;
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return EMPTY;
    return m_weapons[playerId].weapons;
}

bool PlayerWeaponPersistService::areWeaponsApplied(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    return m_weapons[playerId].applied;
}

void PlayerWeaponPersistService::markWeaponsApplied(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_weapons[playerId].applied = true;
}

void PlayerWeaponPersistService::subscribeWeaponsLoaded(WeaponsObserver observer)
{
    m_weaponsObservers.push_back(std::move(observer));
}

void PlayerWeaponPersistService::loadWeapons(IPlayer &player, std::vector<std::pair<std::uint8_t, int>> weapons)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_weapons[playerId] = WeaponsCache{true, std::move(weapons)};
    for (const WeaponsObserver &observer : m_weaponsObservers)
        observer(player, m_weapons[playerId].weapons);
}

void PlayerWeaponPersistService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_weapons[playerId] = WeaponsCache{};
}
