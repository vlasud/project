#include "Services/BankService/BankService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <mysqlx/xdevapi.h>

void BankService::deposit(AccountId accountId, std::int64_t amount)
{
    if (amount <= 0)
        return;

    DatabaseManager::throwQuery(
        [accountId, amount](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO bank_account (account_id, balance) VALUES (?, ?) "
                     "ON DUPLICATE KEY UPDATE balance = balance + VALUES(balance)")
                .bind(accountId, amount)
                .execute();
        });
}

void BankService::creditFactionSalaries(int factionId)
{
    DatabaseManager::throwQuery(
        [factionId](mysqlx::Schema schema)
        {
            // Один запрос: каждому члену фракции (в т.ч. оффлайн) — чек на его
            // персональную зарплату; нулевые зарплаты не плодят пустых счетов.
            schema.getSession()
                .sql("INSERT INTO bank_account (account_id, balance) "
                     "SELECT account_id, salary FROM faction_member "
                     "WHERE faction_id = ? AND salary > 0 "
                     "ON DUPLICATE KEY UPDATE balance = balance + VALUES(balance)")
                .bind(factionId)
                .execute();
        });
}

void BankService::getBalance(AccountId accountId, std::function<void(std::int64_t)> callback)
{
    DatabaseManager::selectQuery<std::int64_t>(
        [accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult result = schema.getTable("bank_account")
                                           .select("balance")
                                           .where("account_id = :account")
                                           .limit(1)
                                           .bind("account", accountId)
                                           .execute();
            mysqlx::Row row = result.fetchOne();
            return row ? row.get(0).get<std::int64_t>() : std::int64_t{0};
        },
        [callback = std::move(callback)](std::int64_t balance) { callback(balance); },
        [](const std::string &) { LogManager::log(Error, "BankService: failed to read balance"); });
}
