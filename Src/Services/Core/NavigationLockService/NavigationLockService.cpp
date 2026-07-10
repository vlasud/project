#include "Services/Core/NavigationLockService/NavigationLockService.h"

#include <utility>

void NavigationLockService::acquire(int playerId, std::string reason)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_locks[playerId].locked = true;
    m_locks[playerId].reason = std::move(reason);
    // Оповестить подписчиков (гашение активного GPS-маркера). Список фиксируется в
    // конструкторах систем и во время оповещения не меняется — итерируем как есть.
    for (const AcquireObserver &observer : m_acquiredObservers)
    {
        observer(playerId);
    }
}

void NavigationLockService::release(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_locks[playerId] = Lock{};
}

bool NavigationLockService::isLocked(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return false;
    }
    return m_locks[playerId].locked;
}

const std::string &NavigationLockService::lockReason(int playerId) const
{
    static const std::string empty;
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return empty;
    }
    return m_locks[playerId].reason;
}

void NavigationLockService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_locks[playerId] = Lock{};
}

void NavigationLockService::subscribeAcquired(AcquireObserver observer)
{
    m_acquiredObservers.push_back(std::move(observer));
}
