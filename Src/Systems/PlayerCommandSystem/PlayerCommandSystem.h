#pragma once

#include "../../Services/PlayerCommandService/PlayerCommandService.h"
#include "../BaseSystem.h"
#include "player.hpp"

class PlayerCommandSystem : public BaseSystem, public PlayerTextEventHandler
{
  public:
    PlayerCommandSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerCommandText(IPlayer &player, StringView message) override;

  private:
    PlayerCommandService &m_commandService;
};
