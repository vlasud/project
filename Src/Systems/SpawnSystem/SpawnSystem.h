#pragma once

#include "../ISystem.h"
#include "player.hpp"

struct SpawnSystemPlayerSpawnEventHandler : PlayerSpawnEventHandler
{
    void onPlayerSpawn(IPlayer &player) override;
};

class SpawnSystem : public ISystem
{
  public:
    void link(ICore *core) override;
    void initialize() override;
    void reset() override;

  private:
    SpawnSystemPlayerSpawnEventHandler m_playerSpawnEventHandler;
};
