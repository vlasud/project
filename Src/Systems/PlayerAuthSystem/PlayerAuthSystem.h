#pragma once

#include "../../Services/PlayerAuthService/PlayerAuthService.h"
#include "../../Services/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "../../Services/PlayerDialogService/PlayerDialogService.h"
#include "../BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Classes/classes.hpp>
#include <cstdint>

class PlayerAuthSystem : public BaseSystem,
                         public PlayerConnectEventHandler,
                         public PlayerChangeEventHandler,
                         public ClassEventHandler,
                         public PlayerSpawnEventHandler
{
  public:
    PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

    void onPlayerKeyStateChange(IPlayer &player, uint32_t newKeys, uint32_t oldKeys) override;

    bool onPlayerRequestClass(IPlayer &player, unsigned int classId) override;

    void onPlayerSpawn(IPlayer &player) override;

  private:
    enum class ESex : uint8_t
    {
        MALE = 0,
        FEMALE
    };

    struct LoginData
    {
        int loginAttempts = 0;
        std::string passwordHash;
    };

    struct RegistrationData
    {
        ESex sex = ESex::MALE;
        std::string password;
    };

    void resetState(int playerId);
    void runRegistration(int playerId);
    void runLogin(int playerId);
    void runChooseSex(int playerId);
    void runSelectSkin(int playerId);
    void buildLoginDialogs();
    void buildRegistrationDialogs();
    void finalizeRegistration(IPlayer &player);
    void finalize(IPlayer &player);

    PlayerAuthService &m_authService;
    PlayerConnectionVersionService &m_connectionVersionService;
    PlayerDialogService &m_dialogService;

    std::array<LoginData, MAX_PLAYERS> m_loginData;
    std::array<RegistrationData, MAX_PLAYERS> m_registrationData;

    int m_loginDialogId = -1;
    int m_registrationPasswordDialogId = -1;
    int m_registrationConfirmPassowrdDialogId = -1;
    int m_registrationChooseSexDialog = -1;
};
