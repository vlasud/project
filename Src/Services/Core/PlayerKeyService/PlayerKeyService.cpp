#include "Services/Core/PlayerKeyService/PlayerKeyService.h"

#include <algorithm>
#include <chrono>

namespace
{
// Повторные нажатия одной клавиши чаще порога отбрасываются: человек так не
// печатает, а чит, дёргающий бит каждый синк, не должен спамить обработчики.
constexpr auto KEY_DEBOUNCE = std::chrono::milliseconds(150);
} // namespace

int PlayerKeyService::onPress(std::uint32_t key, Handler handler)
{
    const int id = m_nextId++;
    m_subscriptions.push_back(Subscription{id, key, true, std::move(handler)});
    return id;
}

int PlayerKeyService::onRelease(std::uint32_t key, Handler handler)
{
    const int id = m_nextId++;
    m_subscriptions.push_back(Subscription{id, key, false, std::move(handler)});
    return id;
}

void PlayerKeyService::unsubscribe(int subscriptionId)
{
    m_subscriptions.erase(std::remove_if(m_subscriptions.begin(), m_subscriptions.end(),
                                         [subscriptionId](const Subscription &s) { return s.id == subscriptionId; }),
                          m_subscriptions.end());
}

bool PlayerKeyService::isPressed(int playerId, std::uint32_t key) const
{
    return (m_lastKeys[playerId] & key) == key;
}

// ------------------------------------------------------------------ вызовы PlayerKeySystem

void PlayerKeyService::handleKeyStateChange(IPlayer &player, std::uint32_t newKeys, std::uint32_t oldKeys,
                                            TimePoint now)
{
    const int playerId = player.getID();
    m_lastKeys[playerId] = newKeys;

    const std::uint32_t pressed = newKeys & ~oldKeys;
    const std::uint32_t released = ~newKeys & oldKeys;
    if (pressed == 0 && released == 0)
    {
        return;
    }

    for (std::uint32_t bit = 1; bit != 0; bit <<= 1)
    {
        if (pressed & bit)
        {
            // Антиспам: слишком частые нажатия одной клавиши не доставляются.
            auto &lastByKey = m_lastPress[playerId];
            auto it = lastByKey.find(bit);
            if (it != lastByKey.end() && now - it->second < KEY_DEBOUNCE)
            {
                continue;
            }
            lastByKey[bit] = now;
            dispatch(player, bit, true);
        }
        else if (released & bit)
        {
            dispatch(player, bit, false);
        }
    }
}

void PlayerKeyService::resetPlayer(int playerId)
{
    m_lastKeys[playerId] = 0;
    m_lastPress[playerId].clear();
}

// ------------------------------------------------------------------ private

void PlayerKeyService::dispatch(IPlayer &player, std::uint32_t key, bool press)
{
    // Снапшот обработчиков: колбэк может подписываться/отписываться, не ломая обход.
    m_dispatchScratch.clear();
    for (const Subscription &subscription : m_subscriptions)
    {
        if (subscription.press == press && (subscription.key & key) != 0 && subscription.handler)
        {
            m_dispatchScratch.push_back(subscription.handler);
        }
    }

    for (const Handler &handler : m_dispatchScratch)
    {
        handler(player);
    }
}
