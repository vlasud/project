#include "Services/AdminService/AdminService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "sodium/crypto_pwhash.h"
#include <algorithm>
#include <chrono>
#include <mysqlx/xdevapi.h>
#include <optional>
#include <sodium.h>
#include <utility>

namespace
{
// Анти-брутфорс /alogin: антифлуд команд ловит лишь ИДЕНТИЧНЫЕ строки, а перебор
// идёт разными паролями. Поэтому свой счётчик неудач и временный lockout.
constexpr int MAX_ALOGIN_ATTEMPTS = 5;
constexpr std::chrono::seconds ALOGIN_LOCKOUT{60};

// Пустой хеш для bounds-промахов геттера (возврат по ссылке без UB).
const std::string EMPTY_HASH;
} // namespace

int AdminService::getStoredLevel(int id) const
{
    if (id < 0 || id >= MAX_PLAYERS)
        return 0;
    return m_accounts[id].storedLevel;
}

int AdminService::getEffectiveLevel(int id) const
{
    if (id < 0 || id >= MAX_PLAYERS)
        return 0;
    const Account &account = m_accounts[id];
    return account.loggedIn ? account.storedLevel : 0;
}

bool AdminService::isRegistered(int id) const
{
    if (id < 0 || id >= MAX_PLAYERS)
        return false;
    return m_accounts[id].registered;
}

bool AdminService::isLoggedIn(int id) const
{
    if (id < 0 || id >= MAX_PLAYERS)
        return false;
    return m_accounts[id].loggedIn;
}

const std::string &AdminService::passwordHash(int id) const
{
    if (id < 0 || id >= MAX_PLAYERS)
        return EMPTY_HASH;
    return m_accounts[id].passwordHash;
}

void AdminService::cacheLoaded(int id, int level, bool registered, std::string hash)
{
    if (id < 0 || id >= MAX_PLAYERS)
        return;
    Account &account = m_accounts[id];
    account.storedLevel = std::clamp(level, 0, MAX_LEVEL);
    account.registered = registered;
    account.passwordHash = std::move(hash);
    // Разработчик (уровень из БД >= 6) авторизован сразу, без /alogin: пароль ему
    // не нужен. loggedIn=true без пароля ставится СТРОГО здесь и СТРОГО для
    // уровня 6 из БД — командного пути выдать 6 нет (см. setLevel/setAdmin), так
    // что самоназначить разработчика игрок не может.
    if (account.storedLevel >= DEVELOPER_LEVEL)
        account.loggedIn = true;
}

void AdminService::setLoggedIn(int id, bool value)
{
    if (id < 0 || id >= MAX_PLAYERS)
        return;
    m_accounts[id].loggedIn = value;
}

void AdminService::reset(int id)
{
    if (id < 0 || id >= MAX_PLAYERS)
        return;
    m_accounts[id] = Account{};
}

bool AdminService::isLockedOut(int id, TimePoint now) const
{
    if (id < 0 || id >= MAX_PLAYERS)
        return false;
    return now < m_accounts[id].lockoutUntil;
}

int AdminService::lockoutSecondsLeft(int id, TimePoint now) const
{
    if (id < 0 || id >= MAX_PLAYERS)
        return 0;
    const TimePoint until = m_accounts[id].lockoutUntil;
    if (now >= until)
        return 0;
    // ceil остатка в секундах: «осталось 1 сек.» вместо «0».
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
    return static_cast<int>((ms + 999) / 1000);
}

void AdminService::registerFailedLogin(int id, TimePoint now)
{
    if (id < 0 || id >= MAX_PLAYERS)
        return;
    Account &account = m_accounts[id];
    if (++account.failedLogins >= MAX_ALOGIN_ATTEMPTS)
    {
        account.lockoutUntil = now + ALOGIN_LOCKOUT;
        account.failedLogins = 0;
    }
}

void AdminService::clearLoginFails(int id)
{
    if (id < 0 || id >= MAX_PLAYERS)
        return;
    m_accounts[id].failedLogins = 0;
    m_accounts[id].lockoutUntil = TimePoint{};
}

void AdminService::setLevel(IPlayer &target, std::int64_t accountId, int level)
{
    const int id = target.getID();
    // Клампим к MAX_ASSIGNABLE_LEVEL (а не MAX_LEVEL): через команду уровень 6
    // НЕДОСТИЖИМ даже при будущем неверном вызове — 6 приходит только из БД.
    const int clamped = std::clamp(level, 0, MAX_ASSIGNABLE_LEVEL);

    // Кэш — синхронно (команда на главном потоке, target валиден). storedLevel
    // источник правды для эффективного уровня; level==0 снимает и логин.
    if (id >= 0 && id < MAX_PLAYERS)
    {
        m_accounts[id].storedLevel = clamped;
        if (clamped == 0)
            m_accounts[id].loggedIn = false;
    }

    DatabaseManager::throwQuery(
        [accountId, clamped](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO admin_account (account_id, level) VALUES (?, ?) "
                     "ON DUPLICATE KEY UPDATE level = VALUES(level)")
                .bind(accountId, clamped)
                .execute();
        },
        [](const std::string &error) { LogManager::log(Error, "AdminService: failed to set level: " + error); });
}

void AdminService::finalizeRegistration(std::int64_t accountId, int level, std::string plainPassword, std::string ip,
                                        std::function<void(std::string hash)> onDone)
{
    // Регистрация идёт только из /setadmin (1..5), уровень 6 командой не выдаётся.
    const int clamped = std::clamp(level, 1, MAX_ASSIGNABLE_LEVEL);

    // Хеш считается и пишется на ВОРКЕРЕ (Argon2id дорогой). На главный поток
    // возвращается готовая строка-хеш (POD), которой вызывающий сидирует кэш
    // под serial-guard. mysqlx-объекты воркер не отдаёт (контракт).
    DatabaseManager::selectQuery<std::optional<std::string>>(
        [accountId, clamped, password = std::move(plainPassword), ip = std::move(ip)](mysqlx::Schema schema)
        {
            char hash[crypto_pwhash_STRBYTES] = {0};
            crypto_pwhash_str(hash, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE);

            schema.getSession()
                .sql("INSERT INTO admin_account (account_id, level, password_hash, registration_ip, registered_at) "
                     "VALUES (?, ?, ?, ?, NOW()) "
                     "ON DUPLICATE KEY UPDATE level = VALUES(level), password_hash = VALUES(password_hash), "
                     "registration_ip = VALUES(registration_ip), registered_at = VALUES(registered_at)")
                .bind(accountId, clamped, std::string(hash), ip)
                .execute();

            return std::optional<std::string>(std::string(hash));
        },
        [onDone = std::move(onDone)](std::optional<std::string> hash)
        {
            if (hash)
                onDone(std::move(*hash));
        },
        [](const std::string &error)
        { LogManager::log(Error, "AdminService: failed to register admin: " + error); });
}
