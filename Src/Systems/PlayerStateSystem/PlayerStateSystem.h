#pragma once

#include "../../Services/AntiCheatService/AntiCheatService.h"
#include "../../Services/PlayerStateService/PlayerStateService.h"
#include "../BaseSystem.h"
#include "player.hpp"

// Привод сервиса стейта: смены стейта валидируются на событии, спец-экшен
// сверяется на каждом апдейте, нарушения уходят в журнал античита.
class PlayerStateSystem : public BaseSystem,
                          public PlayerChangeEventHandler,
                          public PlayerUpdateEventHandler,
                          public PlayerSpawnEventHandler,
                          public PlayerConnectEventHandler
{
  public:
    PlayerStateSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerStateChange(IPlayer &player, PlayerState newState, PlayerState oldState) override;
    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerStateService &m_stateService;
    AntiCheatService &m_antiCheatService;
};
