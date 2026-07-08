#include "Services/PlayerMoneyPersistService/PlayerMoneyPersistService.h"

bool PlayerMoneyPersistService::isMoneyLoaded(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    return m_money[playerId].loaded;
}

unsigned long long PlayerMoneyPersistService::cachedMoney(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    return m_money[playerId].cash;
}

void PlayerMoneyPersistService::subscribeMoneyLoaded(MoneyObserver observer)
{
    m_moneyObservers.push_back(std::move(observer));
}

void PlayerMoneyPersistService::loadMoney(IPlayer &player, unsigned long long cash)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_money[playerId] = MoneyCache{true, cash};
    for (const MoneyObserver &observer : m_moneyObservers)
        observer(player, cash);
}

void PlayerMoneyPersistService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_money[playerId] = MoneyCache{};
}
