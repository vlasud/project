#include "PlayerAuthSystem.h"

#include "../../Database/DatabaseManager.h"
#include "../../ThreadPool/ThreadPool.h"
#include "mysqlx/xdevapi.h"
#include "sodium/crypto_pwhash.h"
#include "types.hpp"
#include <sodium.h>

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
    int currentConnectionVersion = m_playerConnectionVersionService.getVersion(player.getID());

    DatabaseManager::selectQuery(
        [name = std::move(name)](mysqlx::Schema schema)
        {
            return schema.getTable("player")
                .select("id", "password_hash")
                .where("name = :name")
                .limit(1)
                .bind("name", name)
                .execute();
        },
        [this, currentConnectionVersion, playerId = player.getID()](mysqlx::RowResult result)
        {
            if (m_playerConnectionVersionService.getVersion(playerId) != currentConnectionVersion)
            {
                return;
            }

            if (result.count() == 0)
            {
                runRegistration(playerId);
                return;
            }

            mysqlx::Row row = result.fetchOne();
            m_playerAuthService.setPlayerPasswordHash(playerId, row.get(1).get<std::string>());
            runLogin(playerId);
        });
}

void PlayerAuthSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_playerAuthService.setPlayerAuthenticated(player.getID(), false);
}

void PlayerAuthSystem::runRegistration(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }

    player->sendClientMessage(Colour::White(), "runRegistration");
    std::string name = player->getName().to_string();

    DatabaseManager::throwQuery(
        [name = std::move(name)](mysqlx::Schema schema)
        {
            std::string password = "vlasud2204";
            char hash[crypto_pwhash_STRBYTES];
            crypto_pwhash_str(hash, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE);

            schema.getTable("player").insert("name", "password_hash").values(name, hash).execute();
        });
}

void PlayerAuthSystem::runLogin(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }

    player->sendClientMessage(Colour::White(), "runLogin");

    std::string hash = m_playerAuthService.getPlayerPasswordHash(playerId);
    std::string password = "vlasud2204";

    ThreadPool::Task<bool> task;

    task.func = [password = std::move(password), hash = std::move(hash)]()
    {
        return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) != -1;
    };

    task.callback = [](bool isVerify)
    {
    };

    ThreadPool::addTask(std::move(task));
}
