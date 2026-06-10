#pragma once

#include "Services/Core/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

class PlayerConnectionVersionSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    PlayerConnectionVersionSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;

  private:
    PlayerConnectionVersionService &m_playerConnectionVersionService;
};
