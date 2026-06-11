#pragma once

#include "Services/Core/GangZoneService/GangZoneService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Связывает GangZoneService с компонентом ганг-зон и показывает все зоны
// подключившимся игрокам.
class GangZoneSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    GangZoneSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerConnect(IPlayer &player) override;

  private:
    GangZoneService &m_gangZoneService;
};
