#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "types.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class FriendSystem;

// Друзья — бизнес-сервис (НЕ Core). Чистый контейнер состояния: без обращений к БД
// внутри и без отправки сообщений — это делает FriendSystem.
//
// Держит две вещи:
//  * СПИСОК ДРУЗЕЙ игроков онлайн (кэш строк player_friend). Дружба ВЗАИМНАЯ:
//    у обеих сторон друг у друга по записи, поэтому «кто увидит мой вход» — это
//    ровно мой же список друзей, второго индекса не нужно.
//  * ЗАЯВКИ в друзья: живут только в памяти (как звонок), одна входящая на игрока.
//
// Заявки хранятся У АДРЕСАТА списком: их может быть несколько сразу, и отвечают на
// них по ID ОТПРАВИТЕЛЯ (/friendaccept 22). Поэтому одна заявка не блокирует другие —
// занять чужой «слот заявки» и тем закрыть человека от добавления в друзья нельзя.
class FriendService final : public IService
{
    friend FriendSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    // Сколько живёт неотвеченная заявка. Дальше она протухает, и её можно слать снова.
    static constexpr std::chrono::seconds REQUEST_TTL{60};
    // Потолок ВХОДЯЩИХ заявок на игрока: список конечен, но одна заявка чужую больше
    // не вытесняет и не блокирует.
    static constexpr std::size_t MAX_PENDING_REQUESTS = 10;
    // Потолок списка друзей: и диалог остаётся читаемым, и строки в БД не растут без
    // предела.
    static constexpr std::size_t MAX_FRIENDS = 50;

    // Друг в списке. Имя и last_seen — СНИМОК из БД на момент загрузки списка:
    // оффлайн-другу неоткуда взяться в памяти сервера.
    struct Friend
    {
        AccountId accountId = PlayerSessionService::NO_ACCOUNT;
        std::string name;     // ник аккаунта (player.name)
        std::string lastSeen; // «31.07.2026 23:15»; пусто — игрок ни разу не выходил
    };

    struct Request
    {
        int fromId = -1;              // playerId отправителя
        std::uint32_t fromSerial = 0; // его сессия: слот playerId переиспользуется
        AccountId fromAccount = PlayerSessionService::NO_ACCOUNT;
        TimePoint sentAt;
    };

    // --- список друзей ---
    const std::vector<Friend> &friendsOf(int playerId) const;
    bool isFriend(int playerId, AccountId accountId) const;
    std::size_t friendCount(int playerId) const;

    // --- заявки ---
    // Поставить заявку АДРЕСАТУ. false — id невалидны, это заявка самому себе, у
    // адресата уже висит живая заявка ОТ ЭТОГО ЖЕ отправителя либо его список
    // входящих заполнен.
    bool addRequest(int fromId, std::uint32_t fromSerial, AccountId fromAccount, int toId, TimePoint now);
    // Живая заявка адресата ОТ КОНКРЕТНОГО отправителя; nullptr — её нет или протухла.
    const Request *requestFrom(int toId, int fromId, TimePoint now) const;
    // Сколько живых входящих заявок у игрока (для подсказки в чате).
    std::size_t pendingCount(int toId, TimePoint now) const;
    void clearRequest(int toId, int fromId);
    // Снять протухшие заявки и отдать пары системе — уведомляет она.
    using ExpiredVisitor = std::function<void(int toId, const Request &request)>;
    void expireRequests(TimePoint now, const ExpiredVisitor &visitor);

  private:
    // --- вызывается ТОЛЬКО FriendSystem ---
    void loadFriends(int playerId, std::vector<Friend> friends);
    // Добавить в кэш. false — слот невалиден, дубликат либо упёрлись в MAX_FRIENDS.
    bool addFriend(int playerId, Friend entry);
    bool removeFriend(int playerId, AccountId accountId);
    // Полная чистка слота: список, заявка себе и заявки, отправленные этим игроком
    // другим (иначе на его заявку ответил бы новый владелец слота).
    void resetPlayer(int playerId);

    static bool validId(int playerId);

    std::array<std::vector<Friend>, MAX_PLAYERS> m_friends;
    // Входящие заявки АДРЕСАТА, не длиннее MAX_PENDING_REQUESTS. Поиск по id
    // отправителя линейный — список короткий, а путь холодный (команда игрока).
    std::array<std::vector<Request>, MAX_PLAYERS> m_requests;
};
