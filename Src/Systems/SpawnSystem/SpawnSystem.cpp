#include "SpawnSystem.h"

#include "core.hpp"

void SpawnSystemPlayerSpawnEventHandler::onPlayerSpawn(IPlayer &player)
{
    player.setPosition({2144.5574f, -1303.4647f, 23.8203f});
    player.setSkin(22);
}

void SpawnSystem::link(ICore *core)
{
    core->getPlayers().getPlayerSpawnDispatcher().addEventHandler(&m_playerSpawnEventHandler);
}

void SpawnSystem::initialize()
{
}

void SpawnSystem::reset()
{
}
