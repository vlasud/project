#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"

#include <fmt/format.h>
#include <limits>

namespace
{
// HUD клиента (IPlayer::setMoney) — signed int32; серверный баланс держим в
// unsigned long long, поэтому на клиент уходит значение, зажатое в [0, INT_MAX].
int toClientMoney(unsigned long long amount)
{
    constexpr unsigned long long max = static_cast<unsigned long long>(std::numeric_limits<int>::max());
    return static_cast<int>(amount > max ? max : amount);
}
} // namespace

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
    player.setMoney(toClientMoney(amount));
}

void PlayerMoneyService::giveMoney(IPlayer &player, unsigned long long amount)
{
    const int id = player.getID();
    // Насыщение вместо переполнения: обёрнутый вокруг нуля баланс обнулил бы
    // деньги игрока на легальном начислении.
    const unsigned long long limit = std::numeric_limits<unsigned long long>::max();
    m_state[id] = (amount > limit - m_state[id]) ? limit : m_state[id] + amount;
    player.setMoney(toClientMoney(m_state[id]));
}

bool PlayerMoneyService::canAfford(int playerId, unsigned long long amount) const
{
    return getMoney(playerId) >= amount;
}

bool PlayerMoneyService::take(IPlayer &player, unsigned long long amount)
{
    const int id = player.getID();
    if (!validPlayerId(id) || m_state[id] < amount)
        return false;
    m_state[id] -= amount;
    player.setMoney(toClientMoney(m_state[id]));
    return true;
}

void PlayerMoneyService::syncToClient(IPlayer &player) const
{
    // Только пуш текущего серверного баланса на HUD; m_state не трогаем.
    player.setMoney(toClientMoney(getMoney(player.getID())));
}

void PlayerMoneyService::reset(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    m_state[playerId] = 0;
}
