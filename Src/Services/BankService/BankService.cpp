#include "Services/BankService/BankService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <mysqlx/xdevapi.h>
#include <utility>

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
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "BankService::deposit failed: " + error);
        });
}

void BankService::creditFactionSalaries(int factionId, std::int64_t budgetCap,
                                        std::function<void(std::int64_t creditedTotal)> callback,
                                        std::function<void()> errorCallback)
{
    DatabaseManager::selectQuery<std::int64_t>(
        [factionId, budgetCap](mysqlx::Schema schema) -> std::int64_t
        {
            mysqlx::Session &dbSession = schema.getSession();
            // Транзакция: сумма зарплат И кредит читают ОДИН снимок salary.
            // Кредитуем только если суммарная зарплата укладывается в бюджет —
            // зачисленное == возвращённой сумме списания (печать денег невозможна).
            dbSession.startTransaction();
            try
            {
                mysqlx::SqlResult sum =
                    dbSession.sql("SELECT COALESCE(SUM(salary), 0) FROM faction_member WHERE faction_id = ?")
                        .bind(factionId)
                        .execute();
                mysqlx::Row sumRow = sum.fetchOne();
                const std::int64_t total = sumRow ? sumRow.get(0).get<std::int64_t>() : std::int64_t{0};

                if (total <= 0 || total > budgetCap)
                {
                    // Платить некому или бюджета не хватает — ничего не зачисляем.
                    dbSession.commit();
                    return std::int64_t{0};
                }

                // Кредит из того же снимка salary: каждому члену (в т.ч. оффлайн) —
                // чек на его зарплату; нулевые зарплаты не плодят пустых счетов.
                dbSession
                    .sql("INSERT INTO bank_account (account_id, balance) "
                         "SELECT account_id, salary FROM faction_member "
                         "WHERE faction_id = ? AND salary > 0 "
                         "ON DUPLICATE KEY UPDATE balance = balance + VALUES(balance)")
                    .bind(factionId)
                    .execute();
                dbSession.commit();
                return total;
            }
            catch (...)
            {
                dbSession.rollback();
                throw; // errorCallback залогирует; бюджет в памяти не тронут (кредит=0)
            }
        },
        [callback = std::move(callback)](std::int64_t creditedTotal)
        {
            callback(creditedTotal);
        },
        [errorCallback = std::move(errorCallback)](const std::string &error)
        {
            // Сбой транзакции (rollback): деньги не зачислены, бюджет не тронут.
            LogManager::log(Error, "BankService: faction salary order failed: " + error);
            if (errorCallback)
                errorCallback();
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
        [callback = std::move(callback)](std::int64_t balance)
        {
            callback(balance);
        },
        [](const std::string &)
        {
            LogManager::log(Error, "BankService: failed to read balance");
        });
}
