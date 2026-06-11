#pragma once

#include "Services/Core/PlayerActivityService/PlayerActivityService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник PlayerActivityService: отмечает активность на каждом апдейте,
// раз в секунду гоняет детект пауз таймером и чистит состояние при выходе.
class PlayerActivitySystem : public BaseSystem, public PlayerUpdateEventHandler, public PlayerConnectEventHandler
{
  public:
    PlayerActivitySystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerActivityService &m_activityService;
    TimerService &m_timerService;
};
