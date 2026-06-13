#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

class PlayerSessionSystem;

// Сессия — связка «живое подключение <-> аккаунт в БД». Единственный источник
// правды о том, КТО сейчас за playerId. Стартует PlayerAuthSystem после
// проверки пароля (или регистрации), заканчивается на дисконнекте.
//
// Зачем нужна:
//  * getAccountId() — каждый бизнес-сервис (персонаж, деньги в БД, роли, баны)
//    узнаёт здесь, в какую строку БД писать; никто не хранит свою копию связки;
//  * защита от двойного входа — пока аккаунт в сети, start() для него с другого
//    подключения вернёт false (два игрока в одной строке БД — это дюп);
//  * subscribeStart/subscribeEnd — точки загрузки и сохранения: бизнес-сервис
//    на старте сессии грузит своё, на конце — сохраняет. На сырой дисконнект
//    ради персиста никто не подписывается: системы чистят состояние на
//    дисконнекте, и сохранение из чужого обработчика — гонка за порядком.
//    Конец сессии гарантированно стреляет раньше (PlayerSessionSystem
//    зарегистрирована раньше всех бизнес-систем).
//
// Гонки async-колбэков БД: колбэк, начатый в одной сессии, мог прилететь, когда
// в слоте playerId уже другой игрок (или тот же аккаунт перезашёл). Поэтому у
// сессии есть глобально растущий serial — запомни его при отправке запроса и
// сверь в колбэке:
//
//   const auto serial = m_sessions.get(playerId)->serial;
//   // Запрос И вычитка результата — в задаче на воркере; на главный поток
//   // приходит уже готовое T (RowResult между потоками не передаётся).
//   DatabaseManager::selectQuery<T>(
//       [](mysqlx::Schema schema) { /* execute + fetch -> T */ return data; },
//       [this, playerId, serial](T data) {
//           const auto *s = m_sessions.get(playerId);
//           if (!s || s->serial != serial) return; // уже другая сессия — выбросить
//           ...
//       });
class PlayerSessionService final : public IService
{
    friend PlayerSessionSystem;

  public:
    using AccountId = std::int64_t;
    static constexpr AccountId NO_ACCOUNT = 0;

    struct Session
    {
        AccountId accountId = NO_ACCOUNT;
        std::uint32_t serial = 0; // глобальный номер сессии (не переиспользуется)
        TimePoint startedAt;
    };

    // --- источник правды ---
    bool isActive(int playerId) const;
    AccountId getAccountId(int playerId) const;     // NO_ACCOUNT — не залогинен
    const Session *get(int playerId) const;         // nullptr — сессии нет
    int playerByAccount(AccountId accountId) const; // id игрока или -1 — аккаунт не в сети

    // --- подписки бизнес-сервисов (из конструкторов систем) ---
    // Start: сессия уже активна — можно грузить данные аккаунта.
    // End: сессия ещё активна и игрок ещё подключён — можно сохранять.
    using Observer = std::function<void(IPlayer &, const Session &)>;
    void subscribeStart(Observer observer);
    void subscribeEnd(Observer observer);

    // Вызывает PlayerAuthSystem после успешной проверки пароля/регистрации.
    // false — аккаунт уже в сети у другого игрока (двойной вход), сессия не создана.
    bool start(IPlayer &player, AccountId accountId);

  private:
    // Вызывается PlayerSessionSystem на дисконнекте.
    void end(IPlayer &player);

    std::array<Session, MAX_PLAYERS> m_sessions;
    std::unordered_map<AccountId, int> m_online; // обратный индекс аккаунт -> playerId
    std::uint32_t m_nextSerial = 1;
    std::vector<Observer> m_startObservers;
    std::vector<Observer> m_endObservers;
};
