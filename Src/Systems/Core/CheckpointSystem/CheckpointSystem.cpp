#include "Systems/Core/CheckpointSystem/CheckpointSystem.h"

#include "Log/LogManager.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"

CheckpointSystem::CheckpointSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_checkpointService(serviceRegister.getService<CheckpointService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
    listen(core.getPlayers().getPlayerUpdateDispatcher(), this);

    m_checkpointService.initialize(&m_locationService, &serviceRegister.getService<AntiCheatService>());
}

void CheckpointSystem::initialize(IComponentList *components)
{
    ICheckpointsComponent *checkpoints = components->queryComponent<ICheckpointsComponent>();
    if (!checkpoints)
    {
        LogManager::log(Error, "CheckpointSystem: ICheckpointsComponent is missing, checkpoints are disabled");
        return;
    }
    listen(checkpoints->getEventDispatcher(), this);
}

bool CheckpointSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    m_checkpointService.streamPlayer(player, m_locationService.getPosition(player.getID()), now);
    return true;
}

void CheckpointSystem::onPlayerEnterCheckpoint(IPlayer &player)
{
    m_checkpointService.handleEnter(player, Time::now());
}

void CheckpointSystem::onPlayerLeaveCheckpoint(IPlayer &player)
{
    m_checkpointService.handleLeave(player);
}

void CheckpointSystem::onPlayerEnterRaceCheckpoint(IPlayer &player)
{
    m_checkpointService.handleRaceEnter(player, Time::now());
}

void CheckpointSystem::onPlayerLeaveRaceCheckpoint(IPlayer &player)
{
    m_checkpointService.handleRaceLeave(player);
}

void CheckpointSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_checkpointService.resetPlayer(player.getID());
}
