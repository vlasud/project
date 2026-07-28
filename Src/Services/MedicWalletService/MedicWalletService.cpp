#include "Services/MedicWalletService/MedicWalletService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>

std::int64_t MedicWalletService::balanceOf(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return 0;
    }
    return m_balance[playerId];
}

void MedicWalletService::add(int playerId, AccountId accountId, std::int64_t amount)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    if (accountId == PlayerSessionService::NO_ACCOUNT)
    {
        return; // без сессии копить некуда
    }
    if (amount <= 0)
    {
        return;
    }

    m_balance[playerId] += amount;

    // Write-through в момент начисления (как BankService::deposit) — крах сервера
    // теряет максимум последнее начисление, не весь накопленный кошелёк.
    DatabaseManager::throwQuery(
        [accountId, amount](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO medic_wallet (account_id, balance) VALUES (?, ?) "
                     "ON DUPLICATE KEY UPDATE balance = balance + VALUES(balance)")
                .bind(accountId, amount)
                .execute();
        },
        [accountId](const std::string &error)
        {
            LogManager::log(Error,
                            fmt::format("MedicWalletService: failed to persist deposit for account {}: {}", accountId,
                                        error));
        });
}

std::int64_t MedicWalletService::withdraw(int playerId, AccountId accountId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return 0;
    }
    if (accountId == PlayerSessionService::NO_ACCOUNT)
    {
        return 0;
    }

    const std::int64_t amount = m_balance[playerId];
    if (amount <= 0)
    {
        return 0;
    }

    m_balance[playerId] = 0; // обнулить КЭШ СРАЗУ — анти-дюп двойного клика/висящего диалога

    // Write-through ОТНОСИТЕЛЬНЫМ списанием (balance -= amount) через UPSERT, а НЕ
    // абсолютным SET=0: пул БД (конкурентные воркеры) не гарантирует порядок async-
    // записей. Относительные -amount (withdraw) и +amount (add) КОММУТИРУЮТ, поэтому
    // итог сходится к верному независимо от порядка; абсолютный SET=0 мог бы обогнать
    // ещё не применённый +amount и дать рассинхрон/дюп на релоге. UPSERT, а НЕ чистый
    // UPDATE: у нового аккаунта строки кошелька может ещё не быть (первый в жизни
    // add-INSERT гонится в пуле сессий) — тогда UPDATE задел бы 0 строк и потерял
    // списание -> дубль на следующем логине. INSERT гарантирует строку (баланс
    // временно отрицательный, до прихода +amount от add).
    DatabaseManager::throwQuery(
        [accountId, amount](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO medic_wallet (account_id, balance) VALUES (?, ?) "
                     "ON DUPLICATE KEY UPDATE balance = balance + VALUES(balance)")
                .bind(accountId, -amount)
                .execute();
        },
        [accountId](const std::string &error)
        {
            LogManager::log(Error,
                            fmt::format("MedicWalletService: failed to persist withdrawal for account {}: {}",
                                        accountId, error));
        });

    return amount;
}

void MedicWalletService::load(int playerId, std::int64_t balance)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_balance[playerId] = balance; // только память (загрузка по старту сессии)
}

void MedicWalletService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_balance[playerId] = 0; // teardown памяти на конце сессии — БД НЕ трогаем
}
