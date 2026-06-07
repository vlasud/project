#pragma once

#include "../../Services/PlayerAuthService/PlayerAuthService.h"
#include "../../Services/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "../../Services/PlayerDialogService/PlayerDialogService.h"
#include "../BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Classes/classes.hpp>

class PlayerAuthSystem : public BaseSystem,
                         public PlayerConnectEventHandler,
                         public PlayerChangeEventHandler,
                         public ClassEventHandler
{
  public:
    PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

    void onPlayerKeyStateChange(IPlayer &player, uint32_t newKeys, uint32_t oldKeys) override;

    bool onPlayerRequestClass(IPlayer &player, unsigned int classId) override;

  private:
    void resetPlayerState(int playerId);
    void runRegistration(int playerId);
    void runLogin(int playerId);
    void runSelectSkin(int playerId);
    void buildLoginDialogs();
    void buildRegistrationDialogs();
    void finalize(IPlayer &player);

    PlayerAuthService &m_playerAuthService;
    PlayerConnectionVersionService &m_playerConnectionVersionService;
    PlayerDialogService &m_playerDialogService;

    int m_loginDialogId = -1;
    int m_registrationPasswordDialogId = -1;
    int m_registrationConfirmPassowrdDialogId = -1;

    std::array<bool, MAX_PLAYERS> m_isPlayerSelectSkin{};
    std::array<int, MAX_PLAYERS> m_playerLoginAttempts{};
    std::array<int, MAX_PLAYERS> m_playerSelectedSkinIndex{};
    std::array<std::string, MAX_PLAYERS> m_playerPassword{};
    std::array<std::string, MAX_PLAYERS> m_playersPasswordHash{};
};
