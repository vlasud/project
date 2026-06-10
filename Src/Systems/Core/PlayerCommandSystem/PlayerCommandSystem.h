#pragma once

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

class PlayerCommandSystem : public BaseSystem, public PlayerTextEventHandler
{
  public:
    PlayerCommandSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerCommandText(IPlayer &player, StringView message) override;

  private:
    PlayerCommandService &m_commandService;
};
