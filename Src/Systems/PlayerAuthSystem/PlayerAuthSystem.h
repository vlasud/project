#pragma once

#include "../../Services/PlayerAuthService/PlayerAuthService.h"
#include "../BaseSystem.h"
#include "player.hpp"

class PlayerAuthSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister);
    void initialize() override;
    void reset() override;

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    const PlayerAuthService &m_playerAuthService;
};
