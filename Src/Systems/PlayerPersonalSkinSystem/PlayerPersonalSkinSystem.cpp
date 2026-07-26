#include "Systems/PlayerPersonalSkinSystem/PlayerPersonalSkinSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <mysqlx/xdevapi.h>

PlayerPersonalSkinSystem::PlayerPersonalSkinSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_personalSkinService(serviceRegister.getService<PlayerPersonalSkinService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);

    // Персист ЛИЧНОГО скина (источник правды о гражданском скине аккаунта), а не
    // текущего применённого — тот мог быть органным. Идемпотентный UPDATE по
    // account_id (строка существует: логин/регистрация её создали), поэтому в
    // save-канал: зовётся и на конце сессии (внутри end, до teardown), и
    // периодически автосейвом онлайн-игроков. Сессия в обоих случаях активна
    // (accountId валиден).
    //
    // ИНВАРИАНТ (без serial-гарда тут можно ТОЛЬКО потому, что прогон синхронный):
    // skin берём из слота getSkin(playerId), accountId — из переданной session.
    // Они согласованы, пока прогон save/end синхронен и слот хранит скин ЭТОЙ
    // сессии:
    //  (1) на end session-канал отстреливает РАНЬШЕ сброса слота на дисконнекте —
    //      PlayerSessionSystem зарегистрирована до PlayerPersonalSkinSystem;
    //  (2) на автосейве игрок ОНЛАЙН и слот хранит скин текущей сессии;
    //  (3) auth-флоу не переиспользует слот под другой аккаунт без дисконнекта
    //      (после AUTHENTICATED повторного логина в том же подключении нет).
    // Не делай прогон save/end асинхронным и не меняй порядок регистрации —
    // иначе getSkin(playerId) уедет под чужой accountId; тогда привяжи личный
    // скин к самой Session.
    m_sessionService.subscribeSave(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            const int skin = m_personalSkinService.getSkin(player.getID());
            const PlayerSessionService::AccountId accountId = session.accountId;
            DatabaseManager::throwQuery(
                [accountId, skin](mysqlx::Schema schema)
                {
                    schema.getTable("player")
                        .update()
                        .set("skin", skin)
                        .where("id = :id")
                        .bind("id", accountId)
                        .execute();
                },
                [](const std::string &error)
                { LogManager::log(Error, "PlayerPersonalSkinSystem: failed to persist skin: " + error); });
        });
}

void PlayerPersonalSkinSystem::onPlayerConnect(IPlayer &player)
{
    m_personalSkinService.resetPlayer(player.getID());
}

void PlayerPersonalSkinSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_personalSkinService.resetPlayer(player.getID());
}
