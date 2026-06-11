#pragma once

#include "Services/Core/MapIconService/MapIconService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Связывает MapIconService со стримером и чистит персональные слоты иконок
// при отключении игрока.
class MapIconSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    MapIconSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    MapIconService &m_mapIconService;
};
