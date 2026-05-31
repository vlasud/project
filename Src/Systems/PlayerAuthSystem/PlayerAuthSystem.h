#pragma once

#include "../ISystem.h"
#include "player.hpp"

class PlayerAuthSystem : public ISystem, public PlayerConnectEventHandler
{
  public:
    void link(ICore *core) override;
    void initialize() override;
    void reset() override;

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
};
