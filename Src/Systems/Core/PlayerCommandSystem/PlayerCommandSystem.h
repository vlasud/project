#pragma once

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

class PlayerCommandSystem : public BaseSystem, public PlayerTextEventHandler, public PlayerConnectEventHandler
{
  public:
    PlayerCommandSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerCommandText(IPlayer &player, StringView message) override;
    // Сброс антифлуд-состояния слота, иначе чужой блок/счётчик утечёт в
    // переиспользованный id.
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerCommandService &m_commandService;
};
