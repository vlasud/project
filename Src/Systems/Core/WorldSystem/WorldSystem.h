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

    // Режимы, которым нужен ЖИВОЙ таймер, включаются здесь, а не в конструкторе:
    // ITimersComponent приходит в TimerService только в TimerSystem::initialize
    // (та же ловушка, что документирует AutosaveSystem).
    void initialize(IComponentList *components) override;

    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    WorldService &m_worldService;
};
