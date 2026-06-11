#pragma once

#include "Services/Core/WorldService/WorldService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник WorldService: синхронизирует время/погоду/часы заспавнившимся и
// чистит персональные оверрайды при выходе.
class WorldSystem : public BaseSystem, public PlayerSpawnEventHandler, public PlayerConnectEventHandler
{
  public:
    WorldSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    WorldService &m_worldService;
};
