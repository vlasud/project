#pragma once

#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Systems/BaseSystem.h"
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
    // Нужен для реального рычага: забрать машину у того, кто не выполнил высадку.
    VehicleService &m_vehicleService;
};
