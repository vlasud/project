#pragma once

#include "Services/Core/TimerService/TimerService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Связывает TimerService с компонентом таймеров и отменяет пер-плеерные
// таймеры при отключении игрока.
class TimerSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    TimerSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    TimerService &m_timerService;
};
