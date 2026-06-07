#pragma once

#include "../../Services/PlayerAuthService/PlayerAuthService.h"
#include "../../Services/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
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

    PlayerAuthService &m_playerAuthService;
    PlayerConnectionVersionService &m_playerConnectionVersionService;
};
