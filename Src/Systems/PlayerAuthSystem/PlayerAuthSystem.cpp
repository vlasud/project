#include "PlayerAuthSystem.h"

#include "../../Database/DatabaseManager.h"
#include "types.hpp"

PlayerAuthSystem::PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_playerAuthService(serviceRegister.getService<PlayerAuthService>()),
      m_playerConnectionVersionService(serviceRegister.getService<PlayerConnectionVersionService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void PlayerAuthSystem::onPlayerConnect(IPlayer &player)
{
    if (m_playerAuthService.isPlayerAuthenticated(player.getID()))
    {
        player.kick();
        return;
    }

    std::string name = player.getName().to_string();
    int currentConnectionVersion = m_playerConnectionVersionService.getPlayerConnectionVersion(player.getID());

    DatabaseManager::selectQuery(
        [name = std::move(name)](mysqlx::Schema schema)
        {
            return schema.getTable("player")
                .select("id", "password")
                .where("name = :name")
                .limit(1)
                .bind("name", name)
                .execute();
        },
        [this, currentConnectionVersion, &player](mysqlx::RowResult result)
        {
            if (m_playerConnectionVersionService.getPlayerConnectionVersion(player.getID()) != currentConnectionVersion)
            {
                return;
            }

            if (result.count() == 0)
            {
                runRegistration(player);
            }
            else
            {
                mysqlx::Row row = result.fetchOne();
                m_playerAuthService.setPlayerPassword(player.getID(), row.get(1).get<std::string>());
                runLogin(player);
            }
        });
}

void PlayerAuthSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_playerAuthService.setPlayerAuthenticated(player.getID(), false);
}

void PlayerAuthSystem::runRegistration(IPlayer &player)
{
    player.sendClientMessage(Colour::White(), "Registration");
}

void PlayerAuthSystem::runLogin(IPlayer &player)
{
    player.sendClientMessage(Colour::White(),
                             "Login. Password: " + m_playerAuthService.getPlayerPassword(player.getID()));
}
