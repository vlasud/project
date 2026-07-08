#include "Systems/PlayerWeaponPersistSystem/PlayerWeaponPersistSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <utility>
#include <vector>

PlayerWeaponPersistSystem::PlayerWeaponPersistSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_persistService(serviceRegister.getService<PlayerWeaponPersistService>()),
      m_weaponService(serviceRegister.getService<PlayerWeaponService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    m_sessionService.subscribeStart([this](IPlayer &player, const PlayerSessionService::Session &session)
                                    { loadWeapons(player, session); });
    // Персист в save-канал: идемпотентный REPLACE снимка. Зовётся и на конце
    // сессии (внутри end, до teardown), и периодически автосейвом.
    m_sessionService.subscribeSave([this](IPlayer &player, const PlayerSessionService::Session &session)
                                   { persistWeapons(player, session); });
}

void PlayerWeaponPersistSystem::onPlayerConnect(IPlayer &player)
{
    m_persistService.reset(player.getID());
}

void PlayerWeaponPersistSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_persistService.reset(player.getID());
}

void PlayerWeaponPersistSystem::loadWeapons(IPlayer &player, const PlayerSessionService::Session &session)
{
    if (session.accountId == PlayerSessionService::NO_ACCOUNT)
        return; // без аккаунта негде хранить

    // Несколько строк на аккаунт (weapon, ammo).
    DatabaseManager::selectQuery<std::vector<std::pair<std::uint8_t, int>>>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult rows = schema.getTable("player_weapon")
                                          .select("weapon", "ammo")
                                          .where("account_id = :account")
                                          .bind("account", accountId)
                                          .execute();
            std::vector<std::pair<std::uint8_t, int>> result;
            // weapon/ammo читаем как int64 и сужаем: битый ряд не должен ронять
            // get<int>() (как InventorySystem::loadItems) — giveWeapon сам
            // провалидирует слот по weaponId при применении.
            while (mysqlx::Row row = rows.fetchOne())
            {
                const std::int64_t weapon = row.get(0).get<std::int64_t>();
                const std::int64_t ammo = row.get(1).get<std::int64_t>();
                if (weapon <= 0 || weapon > 255)
                    continue; // мусор вне диапазона weaponId — отбросить
                result.emplace_back(static_cast<std::uint8_t>(weapon), ammo > 0 ? static_cast<int>(ammo) : 0);
            }
            return result;
        },
        [this, playerId = player.getID(), serial = session.serial](std::vector<std::pair<std::uint8_t, int>> weapons)
        {
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            m_persistService.loadWeapons(*player, std::move(weapons));
        },
        [](const std::string &error)
        { LogManager::log(Error, "PlayerWeaponPersistSystem: failed to load weapons: " + error); });
}

void PlayerWeaponPersistSystem::persistWeapons(IPlayer &player, const PlayerSessionService::Session &session)
{
    if (session.accountId == PlayerSessionService::NO_ACCOUNT)
        return;
    const int playerId = player.getID();

    // REPLACE снимка (как InventorySystem::persistItems): удаляем все строки
    // аккаунта и вставляем текущий живой набор одной транзакцией. Гейт — НЕ
    // areWeaponsLoaded (кэш из БД пришёл), а areWeaponsApplied (giveWeapon
    // реально прогнан PlayerAuthSystem'ом на логин-спавне/late-observer'ом):
    // между загрузкой и спавном рантайм-инвентарь ещё пуст (сброшен на
    // коннекте), и снимок этого пустого состояния затёр бы реальное оружие в
    // БД, если сессия завершится ДО спавна (дисконнект в спектейте, автосейв
    // в узком окне между загрузкой и спавном).
    if (!m_persistService.areWeaponsApplied(playerId))
        return;

    std::vector<std::pair<std::uint8_t, int>> weapons;
    m_weaponService.getWeapons(playerId, weapons);

    DatabaseManager::throwQuery(
        [accountId = session.accountId, weapons = std::move(weapons)](mysqlx::Schema schema)
        {
            mysqlx::Table table = schema.getTable("player_weapon");
            mysqlx::Session &dbSession = schema.getSession();
            dbSession.startTransaction();
            try
            {
                table.remove().where("account_id = :account").bind("account", accountId).execute();
                for (const auto &[weaponId, ammo] : weapons)
                    table.insert("account_id", "weapon", "ammo")
                        .values(accountId, static_cast<int>(weaponId), ammo)
                        .execute();
                dbSession.commit();
            }
            catch (...)
            {
                dbSession.rollback(); // не оставляем аккаунт с частичным набором
                throw;                // errorCallback залогирует
            }
        },
        [accountId = session.accountId](const std::string &error)
        {
            LogManager::log(Error, fmt::format("PlayerWeaponPersistSystem: failed to persist weapons of "
                                               "account {}: {}",
                                               accountId, error));
        });
}
