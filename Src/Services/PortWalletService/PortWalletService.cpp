#include "Services/PortWalletService/PortWalletService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>

std::int64_t PortWalletService::balanceOf(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return 0;
    }
    return m_balance[playerId];
}

void PortWalletService::add(int playerId, AccountId accountId, std::int64_t amount)
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

    // Write-through в момент начисления (как BankService::deposit) — крах
    // сервера теряет максимум несомую коробку, не весь накопленный кошелёк.
    DatabaseManager::throwQuery(
        [accountId, amount](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO port_wallet (account_id, balance) VALUES (?, ?) "
                     "ON DUPLICATE KEY UPDATE balance = balance + VALUES(balance)")
                .bind(accountId, amount)
                .execute();
        },
        [accountId](const std::string &error)
        {
            LogManager::log(Error,
                            fmt::format("PortWalletService: failed to persist deposit for account {}: {}", accountId,
                                        error));
        });
}

std::int64_t PortWalletService::withdraw(int playerId, AccountId accountId)
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

    // Write-through ОТНОСИТЕЛЬНЫМ списанием (balance -= amount), а НЕ абсолютным
    // SET=0: пул БД (16 сессий, конкурентные воркеры) не гарантирует порядок async-
    // записей. Относительные -amount (withdraw) и +amount (add) КОММУТИРУЮТ, поэтому
    // итог сходится к верному независимо от порядка; абсолютный SET=0 мог бы обогнать
    // ещё не применённый +amount и дать рассинхрон/дюп на релоге. Чистый UPDATE (не
    // UPSERT): строка всегда существует (withdraw гейтится balance>0), нет строки —
    // безопасный no-op (списывать нечего).
    DatabaseManager::throwQuery(
        [accountId, amount](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("UPDATE port_wallet SET balance = balance - ? WHERE account_id = ?")
                .bind(amount, accountId)
                .execute();
        },
        [accountId](const std::string &error)
        {
            LogManager::log(Error,
                            fmt::format("PortWalletService: failed to persist withdrawal for account {}: {}",
                                        accountId, error));
        });

    return amount;
}

void PortWalletService::load(int playerId, std::int64_t balance)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_balance[playerId] = balance; // только память (загрузка по старту сессии)
}

void PortWalletService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_balance[playerId] = 0; // teardown памяти на конце сессии — БД НЕ трогаем
}
