#include "Systems/PlayerPersonalSkinSystem/PlayerPersonalSkinSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <mysqlx/xdevapi.h>

PlayerPersonalSkinSystem::PlayerPersonalSkinSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_personalSkinService(serviceRegister.getService<PlayerPersonalSkinService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    // Персист на конце сессии: пишем ЛИЧНЫЙ скин (источник правды о гражданском
    // скине аккаунта), а не текущий применённый — тот мог быть органным.
    // Сессия на этот момент ещё активна (accountId валиден). UPDATE по
    // account_id — целевая строка существует (логин/регистрация её создали).
    //
    // ИНВАРИАНТ (без serial-гарда тут можно ТОЛЬКО потому, что он синхронный):
    // skin берём из слота getSkin(playerId), accountId — из закрываемой session.
    // Они согласованы, пока end() синхронен и слот ещё хранит скин ЭТОЙ сессии:
    //  (1) session end отстреливает РАНЬШЕ сброса слота на дисконнекте —
    //      PlayerSessionSystem зарегистрирована до PlayerPersonalSkinSystem;
    //  (2) auth-флоу не переиспользует слот под другой аккаунт без дисконнекта
    //      (после AUTHENTICATED повторного логина в том же подключении нет).
    // Не делай end()/применение скина асинхронным и не меняй порядок регистрации
    // — иначе getSkin(playerId) уедет под чужой accountId; тогда привяжи личный
    // скин к самой Session.
    m_sessionService.subscribeEnd(
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
