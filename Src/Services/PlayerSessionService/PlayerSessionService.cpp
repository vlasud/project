#include "Services/PlayerSessionService/PlayerSessionService.h"

#include "Log/LogManager.h"
#include <chrono>
#include <fmt/format.h>

bool PlayerSessionService::isActive(int playerId) const
{
    return playerId >= 0 && playerId < MAX_PLAYERS && m_sessions[playerId].accountId != NO_ACCOUNT;
}

PlayerSessionService::AccountId PlayerSessionService::getAccountId(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return NO_ACCOUNT;
    return m_sessions[playerId].accountId;
}

const PlayerSessionService::Session *PlayerSessionService::get(int playerId) const
{
    if (!isActive(playerId))
        return nullptr;
    return &m_sessions[playerId];
}

int PlayerSessionService::playerByAccount(AccountId accountId) const
{
    const auto it = m_online.find(accountId);
    return it != m_online.end() ? it->second : -1;
}

void PlayerSessionService::subscribeStart(Observer observer)
{
    m_startObservers.push_back(std::move(observer));
}

void PlayerSessionService::subscribeEnd(Observer observer)
{
    m_endObservers.push_back(std::move(observer));
}

bool PlayerSessionService::start(IPlayer &player, AccountId accountId)
{
    if (accountId == NO_ACCOUNT)
        return false;

    const int playerId = player.getID();
    const int onlineAs = playerByAccount(accountId);
    if (onlineAs >= 0 && onlineAs != playerId)
        return false; // двойной вход

    // Повторный start того же игрока (перелогин без дисконнекта) — закрываем
    // старую сессию по-честному, с сохранением у подписчиков.
    if (isActive(playerId))
    {
        LogManager::log(Warning, fmt::format("PlayerSessionService: player {} restarts session (account {} -> {})",
                                             playerId, m_sessions[playerId].accountId, accountId));
        end(player);
    }

    Session &session = m_sessions[playerId];
    session.accountId = accountId;
    session.serial = m_nextSerial++;
    session.startedAt = std::chrono::steady_clock::now();
    m_online[accountId] = playerId;

    for (const Observer &observer : m_startObservers)
        observer(player, session);
    return true;
}

void PlayerSessionService::end(IPlayer &player)
{
    const int playerId = player.getID();
    if (!isActive(playerId))
        return; // не залогинился — нечего заканчивать

    // Подписчики сохраняются, пока сессия ещё числится активной (их колбэки
    // могут читать getAccountId этого же игрока).
    const Session session = m_sessions[playerId];
    for (const Observer &observer : m_endObservers)
        observer(player, session);

    m_online.erase(session.accountId);
    m_sessions[playerId] = Session{};
}
