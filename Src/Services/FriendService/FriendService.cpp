#include "Services/FriendService/FriendService.h"

#include <algorithm>
#include <utility>

namespace
{
const std::vector<FriendService::Friend> EMPTY_FRIENDS;
} // namespace

bool FriendService::validId(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}

// ------------------------------------------------------------------ список друзей

const std::vector<FriendService::Friend> &FriendService::friendsOf(int playerId) const
{
    if (!validId(playerId))
    {
        return EMPTY_FRIENDS;
    }
    return m_friends[playerId];
}

bool FriendService::isFriend(int playerId, AccountId accountId) const
{
    if (!validId(playerId) || accountId == PlayerSessionService::NO_ACCOUNT)
    {
        return false;
    }
    const std::vector<Friend> &list = m_friends[playerId];
    return std::any_of(list.begin(), list.end(),
                       [accountId](const Friend &entry) { return entry.accountId == accountId; });
}

std::size_t FriendService::friendCount(int playerId) const
{
    return validId(playerId) ? m_friends[playerId].size() : 0;
}

void FriendService::loadFriends(int playerId, std::vector<Friend> friends)
{
    if (!validId(playerId))
    {
        return;
    }
    // Потолок соблюдаем и на загрузке: в БД строк могло накопиться больше, чем
    // разрешает текущий MAX_FRIENDS (правка константы вниз не должна ломать диалог).
    if (friends.size() > MAX_FRIENDS)
    {
        friends.resize(MAX_FRIENDS);
    }
    m_friends[playerId] = std::move(friends);
}

bool FriendService::addFriend(int playerId, Friend entry)
{
    if (!validId(playerId) || entry.accountId == PlayerSessionService::NO_ACCOUNT)
    {
        return false;
    }
    if (m_friends[playerId].size() >= MAX_FRIENDS || isFriend(playerId, entry.accountId))
    {
        return false;
    }
    m_friends[playerId].push_back(std::move(entry));
    return true;
}

bool FriendService::removeFriend(int playerId, AccountId accountId)
{
    if (!validId(playerId))
    {
        return false;
    }
    std::vector<Friend> &list = m_friends[playerId];
    const auto it = std::find_if(list.begin(), list.end(),
                                 [accountId](const Friend &entry) { return entry.accountId == accountId; });
    if (it == list.end())
    {
        return false;
    }
    list.erase(it);
    return true;
}

// ------------------------------------------------------------------ заявки

bool FriendService::addRequest(int fromId, std::uint32_t fromSerial, AccountId fromAccount, int toId, TimePoint now)
{
    if (!validId(fromId) || !validId(toId) || fromId == toId)
    {
        return false;
    }
    // Повторная заявка от ТОГО ЖЕ отправителя не нужна: она уже висит.
    if (requestFrom(toId, fromId, now) != nullptr)
    {
        return false;
    }
    std::vector<Request> &list = m_requests[toId];
    // Протухшие подчищаем здесь же: иначе список упирался бы в потолок из мёртвых
    // заявок раньше, чем до них доберётся тик.
    list.erase(std::remove_if(list.begin(), list.end(),
                              [now](const Request &request) { return now - request.sentAt >= REQUEST_TTL; }),
               list.end());
    if (list.size() >= MAX_PENDING_REQUESTS)
    {
        return false;
    }

    Request request;
    request.fromId = fromId;
    request.fromSerial = fromSerial;
    request.fromAccount = fromAccount;
    request.sentAt = now;
    list.push_back(request);
    return true;
}

const FriendService::Request *FriendService::requestFrom(int toId, int fromId, TimePoint now) const
{
    if (!validId(toId) || !validId(fromId))
    {
        return nullptr;
    }
    const std::vector<Request> &list = m_requests[toId];
    const auto it = std::find_if(list.begin(), list.end(),
                                 [fromId, now](const Request &request) {
                                     return request.fromId == fromId && now - request.sentAt < REQUEST_TTL;
                                 });
    return it != list.end() ? &(*it) : nullptr;
}

std::size_t FriendService::pendingCount(int toId, TimePoint now) const
{
    if (!validId(toId))
    {
        return 0;
    }
    const std::vector<Request> &list = m_requests[toId];
    return static_cast<std::size_t>(std::count_if(
        list.begin(), list.end(), [now](const Request &request) { return now - request.sentAt < REQUEST_TTL; }));
}

void FriendService::clearRequest(int toId, int fromId)
{
    if (!validId(toId))
    {
        return;
    }
    std::vector<Request> &list = m_requests[toId];
    list.erase(std::remove_if(list.begin(), list.end(),
                              [fromId](const Request &request) { return request.fromId == fromId; }),
               list.end());
}

void FriendService::expireRequests(TimePoint now, const ExpiredVisitor &visitor)
{
    // Снимаем протухшие СНАЧАЛА, уведомляем потом: наблюдатель шлёт сообщения и
    // видеть протухшую заявку живой он не должен, а мутировать список во время
    // обхода — тем более.
    std::vector<std::pair<int, Request>> expired;
    for (int id = 0; id < MAX_PLAYERS; ++id)
    {
        std::vector<Request> &list = m_requests[id];
        if (list.empty())
        {
            continue;
        }
        for (const Request &request : list)
        {
            if (now - request.sentAt >= REQUEST_TTL)
            {
                expired.emplace_back(id, request);
            }
        }
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [now](const Request &request) { return now - request.sentAt >= REQUEST_TTL; }),
                   list.end());
    }

    if (!visitor)
    {
        return;
    }
    for (const auto &[toId, request] : expired)
    {
        visitor(toId, request);
    }
}

void FriendService::resetPlayer(int playerId)
{
    if (!validId(playerId))
    {
        return;
    }
    m_friends[playerId].clear();
    m_requests[playerId].clear();
    // Заявки, отправленные ЭТИМ игроком другим, тоже снимаем: иначе на неё ответил бы
    // новый владелец слота, а адресат подружился бы с посторонним.
    for (int id = 0; id < MAX_PLAYERS; ++id)
    {
        std::vector<Request> &list = m_requests[id];
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [playerId](const Request &request) { return request.fromId == playerId; }),
                   list.end());
    }
}
