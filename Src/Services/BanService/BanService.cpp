#include "Services/BanService/BanService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <mysqlx/xdevapi.h>
#include <utility>

void BanService::banAccount(std::int64_t accountId, int days, std::string reason, std::int64_t byAccountId,
                            std::string ip)
{
    // banned_until = NOW() + INTERVAL ? DAY. days провалидирован вызывающим
    // (1..365); сам срок считает БД от своего NOW(), не клиент. UPSERT: повторный
    // бан того же аккаунта перетирает срок/причину/IP/время.
    DatabaseManager::throwQuery(
        [accountId, days, reason = std::move(reason), byAccountId, ip = std::move(ip)](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO ban (account_id, banned_until, reason, banned_by, ip, created_at) "
                     "VALUES (?, NOW() + INTERVAL ? DAY, ?, ?, ?, NOW()) "
                     "ON DUPLICATE KEY UPDATE banned_until = VALUES(banned_until), reason = VALUES(reason), "
                     "banned_by = VALUES(banned_by), ip = VALUES(ip), created_at = VALUES(created_at)")
                .bind(accountId, days, reason, byAccountId, ip)
                .execute();
        },
        [](const std::string &error) { LogManager::log(Error, "BanService: failed to ban account: " + error); });
}
