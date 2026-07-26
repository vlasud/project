#include "Systems/PlayerMoneyPersistSystem/PlayerMoneyPersistSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>

PlayerMoneyPersistSystem::PlayerMoneyPersistSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_persistService(serviceRegister.getService<PlayerMoneyPersistService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);

    m_sessionService.subscribeStart([this](IPlayer &player, const PlayerSessionService::Session &session)
                                    { loadMoney(player, session); });
    // Персист в save-канал: идемпотентный UPSERT снимка. Зовётся и на конце
    // сессии (внутри end, до teardown), и периодически автосейвом.
    m_sessionService.subscribeSave([this](IPlayer &player, const PlayerSessionService::Session &session)
                                   { persistMoney(player, session); });
}

void PlayerMoneyPersistSystem::onPlayerConnect(IPlayer &player)
{
    m_persistService.reset(player.getID());
}

void PlayerMoneyPersistSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_persistService.reset(player.getID());
}

void PlayerMoneyPersistSystem::loadMoney(IPlayer &player, const PlayerSessionService::Session &session)
{
    if (session.accountId == PlayerSessionService::NO_ACCOUNT)
        return; // без аккаунта негде хранить

    // Один снимок на аккаунт (PK account_id), нет строки -> 0 (новый аккаунт —
    // стартовой эмиссии больше нет, см. Docs/Persistence.md).
    DatabaseManager::selectQuery<unsigned long long>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult result = schema.getTable("player_money")
                                           .select("cash")
                                           .where("account_id = :account")
                                           .limit(1)
                                           .bind("account", accountId)
                                           .execute();
            if (mysqlx::Row row = result.fetchOne())
            {
                // cash — БД BIGINT (может быть отрицательным в схеме), но
                // серверный баланс unsigned; отрицательное значение в столбце —
                // порча данных (ручная правка БД) и клампится к нулю.
                const std::int64_t signedCash = row.get(0).get<std::int64_t>();
                return signedCash > 0 ? static_cast<unsigned long long>(signedCash) : 0ULL;
            }
            return 0ULL;
        },
        [this, playerId = player.getID(), serial = session.serial](unsigned long long cash)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/другая сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            // Наличные (в отличие от оружия) на спавне НЕ сбрасываются — пишем
            // сразу в авторитетный сервис (как InventorySystem::loadItems), не
            // дожидаясь логин-спавна. Иначе isMoneyLoaded уже true, а getMoney()
            // всё ещё на дефолте — save-guard в persistMoney принял бы это за
            // реальный ноль и затёр бы баланс в БД до применения на спавне.
            m_moneyService.setMoney(*player, cash);
            m_persistService.loadMoney(*player, cash);
        },
        [](const std::string &error)
        { LogManager::log(Error, "PlayerMoneyPersistSystem: failed to load money: " + error); });
}

void PlayerMoneyPersistSystem::persistMoney(IPlayer &player, const PlayerSessionService::Session &session)
{
    if (session.accountId == PlayerSessionService::NO_ACCOUNT)
        return;
    const int playerId = player.getID();

    // Сохраняем ТОЛЬКО если загрузка этого слота завершилась — иначе ещё не
    // загруженное состояние затёрло бы реальный баланс в БД (сбой загрузки/
    // дисконнект до её колбэка).
    if (!m_persistService.isMoneyLoaded(playerId))
        return;

    const unsigned long long cash = m_moneyService.getMoney(playerId);
    DatabaseManager::throwQuery(
        [accountId = session.accountId, cash](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO player_money (account_id, cash) VALUES (?, ?) "
                     "ON DUPLICATE KEY UPDATE cash = VALUES(cash)")
                .bind(accountId, static_cast<std::int64_t>(cash))
                .execute();
        },
        [accountId = session.accountId](const std::string &error)
        {
            LogManager::log(Error, fmt::format("PlayerMoneyPersistSystem: failed to persist money of "
                                               "account {}: {}",
                                               accountId, error));
        });
}
