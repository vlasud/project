#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>

// Источник правды об админ-доступе игроков онлайн. Уровень и пароль —
// СЕРВЕРНЫЕ факты (таблица admin_account), клиент на них не влияет. Уровень
// 0..6; 0 — не админ. Гейтит команды НЕ сохранённый уровень, а ЭФФЕКТИВНЫЙ:
// getEffectiveLevel == 0, пока в этой сессии не выполнен /alogin (пароль
// доказан). Так украденная сессия/реслот слота не даёт прав без пароля.
//
// Уровень 6 («Разработчик») — абсолютный максимум, выставляется ТОЛЬКО прямым
// INSERT в БД: командного пути выдать 6 нет (/setadmin ограничен 0..5). Для 6
// пароль не нужен — он авто-логинится в cacheLoaded (см. ниже), поэтому без
// пароля loggedIn=true ставится СТРОГО для уровня из БД, а не из команды.
//
// Лайфцикл слота — через сессию (AdminSystem): на старте сессии грузится
// storedLevel + хеш пароля (cacheLoaded), на конце — reset. Запись в БД —
// write-through (setLevel) и регистрация (finalizeRegistration), кэш
// обновляется после успеха запроса.
//
// Хеш пароля — строка libsodium crypto_pwhash_str (ASCII). Верификация —
// строго на воркере (Argon2id дорогой), здесь хранится только сам хеш для
// передачи в задачу-воркер. Анти-брутфорс /alogin (разные пароли антифлуд
// команд не ловит) — пер-игрок счётчик неудач + временный lockout.
class AdminService final : public IService
{
  public:
    static constexpr int DEVELOPER_LEVEL = 6;        // «Разработчик»: авто-логин без пароля, только из БД
    static constexpr int MAX_LEVEL = 6;              // абсолютный максимум (загрузка из БД, имена уровней)
    static constexpr int MAX_ASSIGNABLE_LEVEL = 5;   // максимум, который может выдать /setadmin

    // --- источник правды (геттеры bounds-guard по id) ---
    int getStoredLevel(int id) const;            // 0..6 из БД (сессии)
    int getEffectiveLevel(int id) const;         // loggedIn ? storedLevel : 0 — ИМЕННО это гейтит команды
    bool isRegistered(int id) const;             // есть ли админ-пароль
    bool isLoggedIn(int id) const;               // /alogin успешен в этой сессии
    const std::string &passwordHash(int id) const; // для верификации на воркере

    // --- лайфцикл слота (AdminSystem) ---
    // Сидирование на старте сессии: уровень/наличие пароля/хеш из БД.
    void cacheLoaded(int id, int level, bool registered, std::string hash);
    void setLoggedIn(int id, bool value);
    void reset(int id);

    // --- анти-брутфорс /alogin ---
    bool isLockedOut(int id, TimePoint now) const;
    int lockoutSecondsLeft(int id, TimePoint now) const;
    // Инкремент неудач; при достижении порога ставит lockout и сбрасывает счётчик.
    void registerFailedLogin(int id, TimePoint now);
    void clearLoginFails(int id);

    // --- запись в БД (источник правды + write-through кэша) ---
    // Выдать/снять уровень (UPSERT level по accountId). level==0 ещё и снимает
    // loggedIn. Кэш слота target обновляется СИНХРОННО (target валиден на момент
    // вызова — команда на главном потоке), запись в БД — write-through.
    void setLevel(IPlayer &target, std::int64_t accountId, int level);
    // Завершить регистрацию админа: на воркере захешировать пароль и записать
    // (level, password_hash, registration_ip, registered_at=NOW()). На успех
    // вызывает onDone(hash) на ГЛАВНОМ потоке — там вызывающий (с serial-guard
    // по своей сессии) сидирует кэш через cacheLoaded и логинит. accountId нужен
    // для UPSERT строки. plainPassword/ip/hash передаются по значению —
    // mysqlx-объекты границу потока не пересекают (контракт DatabaseManager).
    void finalizeRegistration(std::int64_t accountId, int level, std::string plainPassword, std::string ip,
                              std::function<void(std::string hash)> onDone);

  private:
    struct Account
    {
        int storedLevel = 0;      // 0..6 из БД
        bool registered = false;  // есть ли админ-пароль
        bool loggedIn = false;    // /alogin успешен В ЭТОЙ сессии
        std::string passwordHash; // libsodium, загружен на старте сессии
        int failedLogins = 0;     // анти-брутфорс
        TimePoint lockoutUntil;   // блок /alogin до этого момента
    };

    std::array<Account, MAX_PLAYERS> m_accounts;
};
