#pragma once

#include "../../Services/AntiCheatService/AntiCheatService.h"
#include "../../Services/PlayerVelocityService/PlayerVelocityService.h"
#include "../BaseSystem.h"
#include "player.hpp"

// Привод сервиса скорости: сэмплирует velocity из принятых позиций на каждом
// апдейте игрока, устойчивые превышения пишет в журнал античита.
class PlayerVelocitySystem : public BaseSystem, public PlayerUpdateEventHandler, public PlayerConnectEventHandler
{
  public:
    PlayerVelocitySystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerVelocityService &m_velocityService;
    AntiCheatService &m_antiCheatService;
};
