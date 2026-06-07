#include "PlayerAuthSystem.h"

#include "../../Database/DatabaseManager.h"
#include "../../Log/LogManager.h"
#include "../../ThreadPool/ThreadPool.h"
#include "../../Utils/Encoding/Encoding.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "core.hpp"
#include "fmt/base.h"
#include "mysqlx/xdevapi.h"
#include "sodium/crypto_pwhash.h"
#include "types.hpp"
#include <fmt/format.h>
#include <sodium.h>

PlayerAuthSystem::PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_playerAuthService(serviceRegister.getService<PlayerAuthService>()),
      m_playerConnectionVersionService(serviceRegister.getService<PlayerConnectionVersionService>()),
      m_playerDialogService(serviceRegister.getService<PlayerDialogService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    buildLoginDialogs();
    buildRegistrationDialogs();
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
    m_playerDialogService.showDialog(*player, m_registrationDialogId);
}

void PlayerAuthSystem::runLogin(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    m_playerDialogService.showDialog(*player, m_loginDialogId);
}

void PlayerAuthSystem::buildLoginDialogs()
{
    Dialog loginDialog;
    loginDialog.style = DialogStyle_PASSWORD;
    loginDialog.title = Encoding::utf8Tocp1251("Авторизация");
    loginDialog.body = Encoding::utf8Tocp1251("Введите свой пароль");

    loginDialog.leftButton = Encoding::utf8Tocp1251("Далее");
    loginDialog.leftAction = [this](int playerId, int, StringView text)
    {
        IPlayer *player = m_core.getPlayers().get(playerId);

        if (text.size() < 8)
        {
            m_playerDialogService.showDialog(*player, m_loginDialogId);
            player->sendClientMessage(Colour::White(), Encoding::utf8Tocp1251("Неверный пароль!"));
            return;
        }

        const std::string password = text.to_string();
        const std::string hash = m_playerAuthService.getPlayerPasswordHash(playerId);

        ThreadPool::Task<bool> task;
        task.func = [password = std::move(password), hash = std::move(hash)]()
        {
            return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) == 0;
        };
        task.callback = [this, playerId](bool verifyResult)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);

            if (!verifyResult)
            {
                m_playerDialogService.showDialog(*player, m_loginDialogId);
                player->sendClientMessage(Colour::White(), Encoding::utf8Tocp1251("Неверный пароль!"));
                return;
            }

            finalize(*player);
        };

        ThreadPool::addTask(task);
    };

    loginDialog.rightButton = Encoding::utf8Tocp1251("Выйти");
    loginDialog.rightAction = [this](int playerId, ...)
    {
        m_core.getPlayers().get(playerId)->kick();
    };

    m_loginDialogId = m_playerDialogService.buildDialog(std::move(loginDialog));
}

void PlayerAuthSystem::buildRegistrationDialogs()
{
    Dialog registrationDialog;
    registrationDialog.style = DialogStyle_PASSWORD;
    registrationDialog.title = Encoding::utf8Tocp1251("Регистрация");
    registrationDialog.body = Encoding::utf8Tocp1251("Придумайте и введите пароль\nМинимум 8 символов");

    registrationDialog.leftButton = Encoding::utf8Tocp1251("Далее");
    registrationDialog.leftAction = [this](int playerId, int, StringView text)
    {
        IPlayer *player = m_core.getPlayers().get(playerId);

        if (text.size() < 8)
        {
            player->sendClientMessage(Colour::White(), Encoding::utf8Tocp1251("Минимум 8 символов!"));
            m_playerDialogService.showDialog(*player, m_registrationDialogId);
            return;
        }

        const std::string name = player->getName().to_string();
        const std::string password = text.to_string();

        DatabaseManager::throwQuery(
            [name = std::move(name), password = std::move(password)](mysqlx::Schema schema)
            {
                char hash[crypto_pwhash_STRBYTES];
                crypto_pwhash_str(hash, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                  crypto_pwhash_MEMLIMIT_INTERACTIVE);

                schema.getTable("player").insert("name", "password_hash").values(name, hash).execute();
            });

        finalize(*player);
    };

    registrationDialog.rightButton = Encoding::utf8Tocp1251("Выйти");
    registrationDialog.rightAction = [this](int playerId, ...)
    {
        m_core.getPlayers().get(playerId)->kick();
    };

    m_registrationDialogId = m_playerDialogService.buildDialog(std::move(registrationDialog));
}

void PlayerAuthSystem::finalize(IPlayer &player)
{
    player.spawn();
    player.setPosition({2144.5574, -1303.4647, 23.8203});
    player.setSkin(22);
    player.sendClientMessage(Colour::White(), Encoding::utf8Tocp1251("Добро пожаловать!"));
}
