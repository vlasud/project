#include "PlayerAuthSystem.h"

#include "../../Database/DatabaseManager.h"
#include "../../Log/LogManager.h"
#include "../../ThreadPool/ThreadPool.h"
#include "../../Utils/Encoding/Encoding.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "core.hpp"
#include "mysqlx/xdevapi.h"
#include "sodium/crypto_pwhash.h"
#include "types.hpp"
#include <fmt/format.h>
#include <sodium.h>
#include <string>

PlayerAuthSystem::PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_authService(serviceRegister.getService<PlayerAuthService>()),
      m_connectionVersionService(serviceRegister.getService<PlayerConnectionVersionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerChangeDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);
}

void PlayerAuthSystem::initialize(IComponentList *components)
{
    IClassesComponent *component = components->queryComponent<IClassesComponent>();
    component->getEventDispatcher().addEventHandler(this);
}

void PlayerAuthSystem::onPlayerConnect(IPlayer &player)
{
    if (m_authService.getAuthState(player.getID()) != PlayerAuthService::EAuthState::UNKNOWN)
    {
        player.sendClientMessage(Colour::White(),
                                 Encoding::utf8Tocp1251("Что-то пошло не так... Попробуйте перезайти"));
        player.kick();
        return;
    }

    resetState(player.getID());
    m_authService.setPlayerAuthenticated(player.getID(), PlayerAuthService::EAuthState::AUTHORIZING);
}

void PlayerAuthSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    resetState(player.getID());
    m_authService.setPlayerAuthenticated(player.getID(), PlayerAuthService::EAuthState::UNKNOWN);
}

void PlayerAuthSystem::onPlayerKeyStateChange(IPlayer &player, uint32_t newKeys, uint32_t oldKeys)
{
    // player.sendClientMessage(Colour::White(), std::to_string(newKeys));
    // static std::array<int, 5> skins = {1, 2, 3, 4, 5};
}

bool PlayerAuthSystem::onPlayerRequestClass(IPlayer &player, unsigned int classId)
{
    if (m_authService.getAuthState(player.getID()) != PlayerAuthService::EAuthState::AUTHORIZING)
    {
        return false;
    }

    player.spawn();
    return true;
}

void PlayerAuthSystem::onPlayerSpawn(IPlayer &player)
{
    if (m_authService.getAuthState(player.getID()) != PlayerAuthService::EAuthState::AUTHORIZING)
    {
        return;
    }

    player.setSpectating(true);
    player.setPosition({2055.8442, -1104.7142, 24.4337});
    player.interpolateCameraPosition({2059.5425, -1104.4227, 30.5487}, {2059.5425, -1104.4227, 30.5487}, 1000,
                                     PlayerCameraCutType::PlayerCameraCutType_Move);
    player.interpolateCameraLookAt({2055.8442, -1104.7142, 30.4337}, {2055.8442, -1104.7142, 30.4337}, 1000,
                                   PlayerCameraCutType::PlayerCameraCutType_Move);

    std::string name = player.getName().to_string();
    const int requestConnectionVersion = m_connectionVersionService.getVersion(player.getID());

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
        [this, requestConnectionVersion, playerId = player.getID()](mysqlx::RowResult result)
        {
            if (m_connectionVersionService.getVersion(playerId) != requestConnectionVersion)
            {
                return;
            }

            if (result.count() == 0)
            {
                runRegistration(playerId);
                return;
            }

            mysqlx::Row row = result.fetchOne();
            m_loginData[playerId].passwordHash = row.get(1).get<std::string>();
            runLogin(playerId);
        });
}

void PlayerAuthSystem::resetState(int playerId)
{
    m_loginData[playerId] = {};
    m_registrationData[playerId] = {};
}

void PlayerAuthSystem::runRegistration(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }
    showRegistrationPasswordDialog(*player);
}

void PlayerAuthSystem::runLogin(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }
    showLoginDialog(*player);
}

void PlayerAuthSystem::runChooseSex(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }
    showChooseSexDialog(*player);
}

void PlayerAuthSystem::runSelectSkin(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }

    finalizeRegistration(*player);
}

void PlayerAuthSystem::showLoginDialog(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_PASSWORD;
    dialog.title = Encoding::utf8Tocp1251("Авторизация");
    dialog.body = Encoding::utf8Tocp1251(fmt::format("Аккаунт: {}\n\nВведите пароль", player.getName().to_string()));
    dialog.leftButton = Encoding::utf8Tocp1251("Далее");
    dialog.rightButton = Encoding::utf8Tocp1251("Выйти");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 player->kick();
                                 return;
                             }

                             if (text.size() < 8)
                             {
                                 player->sendClientMessage(Colour::White(), Encoding::utf8Tocp1251("Неверный пароль!"));
                                 showLoginDialog(*player);
                                 return;
                             }

                             ThreadPool::Task<bool> task;
                             task.func = [password = text.to_string(), hash = m_loginData[playerId].passwordHash]()
                             {
                                 return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) == 0;
                             };
                             task.callback = [this, playerId](bool verified)
                             {
                                 IPlayer *player = m_core.getPlayers().get(playerId);
                                 if (!player)
                                 {
                                     return;
                                 }

                                 if (!verified)
                                 {
                                     player->sendClientMessage(Colour::White(),
                                                               Encoding::utf8Tocp1251("Неверный пароль!"));
                                     showLoginDialog(*player);
                                     return;
                                 }

                                 finalize(*player);
                             };

                             ThreadPool::addTask(std::move(task));
                         });
}

void PlayerAuthSystem::showRegistrationPasswordDialog(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_PASSWORD;
    dialog.title = Encoding::utf8Tocp1251("Регистрация - Пароль");
    dialog.body = Encoding::utf8Tocp1251("Придумайте и введите пароль. Минимум 8 символов");
    dialog.leftButton = Encoding::utf8Tocp1251("Далее");
    dialog.rightButton = Encoding::utf8Tocp1251("Выйти");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 player->kick();
                                 return;
                             }

                             if (text.size() < 8)
                             {
                                 player->sendClientMessage(Colour::White(),
                                                           Encoding::utf8Tocp1251("Минимум 8 символов!"));
                                 showRegistrationPasswordDialog(*player);
                                 return;
                             }

                             m_registrationData[playerId].password = text.to_string();
                             showRegistrationConfirmDialog(*player);
                         });
}

void PlayerAuthSystem::showRegistrationConfirmDialog(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_PASSWORD;
    dialog.title = Encoding::utf8Tocp1251("Регистрация - Подтверждение пароля");
    dialog.body = Encoding::utf8Tocp1251("Повторите введенный пароль. Можете вернуться назад и изменить пароль");
    dialog.leftButton = Encoding::utf8Tocp1251("Далее");
    dialog.rightButton = Encoding::utf8Tocp1251("Назад");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 showRegistrationPasswordDialog(*player);
                                 return;
                             }

                             if (m_registrationData[playerId].password != text.to_string())
                             {
                                 player->sendClientMessage(Colour::White(),
                                                           Encoding::utf8Tocp1251("Пароли не совпадают"));
                                 showRegistrationConfirmDialog(*player);
                                 return;
                             }

                             runChooseSex(playerId);
                         });
}

void PlayerAuthSystem::showChooseSexDialog(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = Encoding::utf8Tocp1251("Регистрация - Пол");
    dialog.body = Encoding::utf8Tocp1251("Выберите пол персонажа");
    dialog.leftButton = Encoding::utf8Tocp1251("Мужской");
    dialog.rightButton = Encoding::utf8Tocp1251("Женский");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int, StringView)
                         {
                             m_registrationData[playerId].sex =
                                 (response == DialogResponse_Left) ? ESex::MALE : ESex::FEMALE;
                             runSelectSkin(playerId);
                         });
}

void PlayerAuthSystem::finalizeRegistration(IPlayer &player)
{
    std::string name = player.getName().to_string();
    std::string password = std::move(m_registrationData[player.getID()].password);

    DatabaseManager::throwQuery(
        [name = std::move(name), password = std::move(password),
         sex = m_registrationData[player.getID()].sex](mysqlx::Schema schema)
        {
            char hash[crypto_pwhash_STRBYTES] = {0};
            crypto_pwhash_str(hash, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE);

            schema.getTable("player")
                .insert("name", "password_hash", "sex")
                .values(name, hash, static_cast<uint8_t>(sex))
                .execute();
        });

    finalize(player);
}

void PlayerAuthSystem::finalize(IPlayer &player)
{
    m_authService.setPlayerAuthenticated(player.getID(), PlayerAuthService::EAuthState::AUTHENTICATED);
    player.setSpectating(false);
    player.setSkin(22);
    player.setPosition({1762.1505, -1896.2495, 13.5621});
}
