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
#include <string>

PlayerAuthSystem::PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_playerAuthService(serviceRegister.getService<PlayerAuthService>()),
      m_playerConnectionVersionService(serviceRegister.getService<PlayerConnectionVersionService>()),
      m_playerDialogService(serviceRegister.getService<PlayerDialogService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    buildLoginDialogs();
    buildRegistrationDialogs();
}

void PlayerAuthSystem::initialize(IComponentList *components)
{
    IClassesComponent *component = components->queryComponent<IClassesComponent>();
    component->getEventDispatcher().addEventHandler(this);
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
            m_playersPasswordHash[playerId] = row.get(1).get<std::string>();
            runLogin(playerId);
        });
}

void PlayerAuthSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    resetPlayerState(player.getID());
}

void PlayerAuthSystem::onPlayerKeyStateChange(IPlayer &player, uint32_t newKeys, uint32_t oldKeys)
{
    if (!m_isPlayerSelectSkin[player.getID()])
    {
        return;
    }

    player.sendClientMessage(Colour::White(), std::to_string(newKeys));
    static std::array<int, 5> skins = {1, 2, 3, 4, 5};
}

bool PlayerAuthSystem::onPlayerRequestClass(IPlayer &player, unsigned int classId)
{
    player.spawn();
    return false;
}

void PlayerAuthSystem::resetPlayerState(int playerId)
{
    m_isPlayerSelectSkin[playerId] = false;
    m_playerLoginAttempts[playerId] = 0;
    m_playerSelectedSkinIndex[playerId] = 0;
    m_playerPassword[playerId].clear();
    m_playersPasswordHash[playerId].clear();
    m_playerAuthService.setPlayerAuthenticated(playerId, false);
}

void PlayerAuthSystem::runRegistration(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    m_playerDialogService.showDialog(*player, m_registrationPasswordDialogId);
}

void PlayerAuthSystem::runLogin(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    m_playerDialogService.showDialog(*player, m_loginDialogId);
}

void PlayerAuthSystem::runSelectSkin(int playerId)
{
    m_isPlayerSelectSkin[playerId] = true;

    IPlayer *player = m_core.getPlayers().get(playerId);
    player->setPosition({2050.9050, -1104.9208, 24.4648});
    player->setCameraPosition({2059.5425, -1104.4227, 24.5487});
    player->setCameraLookAt({2050.9050, -1104.9208, 24.4648}, 2);
}

void PlayerAuthSystem::buildLoginDialogs()
{
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
            const std::string hash = m_playersPasswordHash[playerId];

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
}

void PlayerAuthSystem::buildRegistrationDialogs()
{
    {
        Dialog dialog;
        dialog.style = DialogStyle_PASSWORD;
        dialog.title = Encoding::utf8Tocp1251("Регистрация - Пароль");
        dialog.body = Encoding::utf8Tocp1251("Придумайте и введите пароль            \n\nМинимум 8 символов");

        dialog.leftButton = Encoding::utf8Tocp1251("Далее");
        dialog.leftAction = [this](int playerId, int, StringView text)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);

            if (text.size() < 8)
            {
                player->sendClientMessage(Colour::White(), Encoding::utf8Tocp1251("Минимум 8 символов!"));
                m_playerDialogService.showDialog(*player, m_registrationPasswordDialogId);
                return;
            }

            m_playerPassword[playerId] = text.to_string();

            m_playerDialogService.showDialog(*player, m_registrationConfirmPassowrdDialogId);
        };

        dialog.rightButton = Encoding::utf8Tocp1251("Выйти");
        dialog.rightAction = [this](int playerId, ...)
        {
            m_core.getPlayers().get(playerId)->kick();
        };

        m_registrationPasswordDialogId = m_playerDialogService.buildDialog(std::move(dialog));
    }

    {
        Dialog dialog;
        dialog.style = DialogStyle_PASSWORD;
        dialog.title = Encoding::utf8Tocp1251("Регистрация - Подтверждение пароля");
        dialog.body = Encoding::utf8Tocp1251("Повторите введенный пароль         ");

        dialog.leftButton = Encoding::utf8Tocp1251("Далее");
        dialog.leftAction = [this](int playerId, int, StringView text)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);

            const std::string name = player->getName().to_string();
            const std::string password = text.to_string();

            if (m_playerPassword[playerId] != password)
            {
                player->sendClientMessage(Colour::White(), Encoding::utf8Tocp1251("Пароли не совпадают"));
                m_playerDialogService.showDialog(*player, m_registrationConfirmPassowrdDialogId);
                return;
            }

            DatabaseManager::throwQuery(
                [name = std::move(name), password = std::move(password)](mysqlx::Schema schema)
                {
                    char hash[crypto_pwhash_STRBYTES];
                    crypto_pwhash_str(hash, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                      crypto_pwhash_MEMLIMIT_INTERACTIVE);

                    schema.getTable("player").insert("name", "password_hash").values(name, hash).execute();
                });

            runSelectSkin(playerId);
        };

        dialog.rightButton = Encoding::utf8Tocp1251("Назад");
        dialog.rightAction = [this](int playerId, ...)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            m_playerDialogService.showDialog(*player, m_registrationPasswordDialogId);
        };

        m_registrationConfirmPassowrdDialogId = m_playerDialogService.buildDialog(std::move(dialog));
    }
}

void PlayerAuthSystem::finalize(IPlayer &player)
{
    player.spawn();
    player.setPosition({2144.5574, -1303.4647, 23.8203});
    player.setSkin(22);
    player.sendClientMessage(Colour::White(), Encoding::utf8Tocp1251("Добро пожаловать!"));
}
