#pragma once

#include "Services/Core/SpectateService/SpectateService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник SpectateService: свип догона цели по таймеру, возврат/переприменение
// на спавнах, чистка на дисконнекте. Плюс дев-команды /spec [id] и /specoff
// (как и остальные тулзы — до появления системы прав доступны всем).
class SpectateSystem : public BaseSystem, public PlayerSpawnEventHandler, public PlayerConnectEventHandler
{
  public:
    SpectateSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    SpectateService &m_spectateService;
    TimerService &m_timerService;
};
