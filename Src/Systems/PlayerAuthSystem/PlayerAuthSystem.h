#pragma once

#include "../../Services/PlayerAuthService/PlayerAuthService.h"
#include "../../Services/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "../../Services/PlayerDialogService/PlayerDialogService.h"
#include "../BaseSystem.h"
#include "player.hpp"

class PlayerAuthSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    void runRegistration(int playerId);
    void runLogin(int playerId);
    void buildLoginDialogs();
    void buildRegistrationDialogs();
    void finalize(IPlayer &player);

    PlayerAuthService &m_playerAuthService;
    PlayerConnectionVersionService &m_playerConnectionVersionService;
    PlayerDialogService &m_playerDialogService;

    int m_loginDialogId = -1;
    int m_registrationDialogId = -1;
};
