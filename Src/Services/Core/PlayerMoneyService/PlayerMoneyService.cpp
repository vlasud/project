#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"

#include <fmt/format.h>

unsigned long long PlayerMoneyService::getMoney(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return 0;
    }
    return m_state[playerId];
}

void PlayerMoneyService::setMoney(IPlayer &player, unsigned long long amount)
{
    m_state[player.getID()] = amount;
    player.setMoney(amount);
}

void PlayerMoneyService::giveMoney(IPlayer &player, unsigned long long amount)
{
    m_state[player.getID()] += amount;
    player.setMoney(amount);
}

void PlayerMoneyService::reset(int playerId)
{
    m_state[playerId] = 0;
}
