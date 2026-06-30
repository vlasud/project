#include "Services/SpawnChoiceService/SpawnChoiceService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>

SpawnChoiceService::Choice SpawnChoiceService::getChoice(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return Choice::Station;
    return m_choice[playerId];
}

void SpawnChoiceService::setChoice(IPlayer &player, AccountId accountId, Choice choice)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    if (accountId == PlayerSessionService::NO_ACCOUNT)
        return; // без сессии выбору некуда сохраняться

    m_choice[playerId] = choice;

    // Write-through, как членство фракций. REPLACE (DELETE+INSERT) в ОДНОЙ
    // ТРАНЗАКЦИИ: либо новая строка применилась, либо ничего (rollback) — не
    // остаётся окна со снятым выбором без новой записи.
    DatabaseManager::throwQuery(
        [accountId, choice = static_cast<int>(choice)](mysqlx::Schema schema)
        {
            mysqlx::Table table = schema.getTable("player_spawn");
            mysqlx::Session &dbSession = schema.getSession();
            dbSession.startTransaction();
            try
            {
                table.remove().where("account_id = :account").bind("account", accountId).execute();
                table.insert("account_id", "choice").values(accountId, choice).execute();
                dbSession.commit();
            }
            catch (...)
            {
                dbSession.rollback();
                throw; // errorCallback залогирует; память остаётся (оптимистичный выбор)
            }
        },
        [accountId](const std::string &error)
        {
            LogManager::log(Error,
                            fmt::format("SpawnChoiceService: failed to persist choice of account {}: {}", accountId,
                                        error));
        });
}

void SpawnChoiceService::load(int playerId, Choice choice)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_choice[playerId] = choice; // только память (загрузка по старту сессии)
}

void SpawnChoiceService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_choice[playerId] = Choice::Station;
}
