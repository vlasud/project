#include "Systems/Core/ClassSelectionSystem/ClassSelectionSystem.h"

#include "Log/LogManager.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"

ClassSelectionSystem::ClassSelectionSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_classSelectionService(serviceRegister.getService<ClassSelectionService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerDamageDispatcher().addEventHandler(this);

    m_classSelectionService.initialize(&serviceRegister.getService<AntiCheatService>());
}

void ClassSelectionSystem::initialize(IComponentList *components)
{
    IClassesComponent *classes = components->queryComponent<IClassesComponent>();
    if (!classes)
    {
        LogManager::log(Error, "ClassSelectionSystem: IClassesComponent is missing");
        return;
    }
    classes->getEventDispatcher().addEventHandler(this);
}

bool ClassSelectionSystem::onPlayerRequestClass(IPlayer &player, unsigned int classId)
{
    return m_classSelectionService.handleRequestClass(player, Time::now());
}

bool ClassSelectionSystem::onPlayerRequestSpawn(IPlayer &player)
{
    return m_classSelectionService.handleRequestSpawn(player, Time::now());
}

void ClassSelectionSystem::onPlayerDeath(IPlayer &player, IPlayer *killer, int reason)
{
    m_classSelectionService.handleDeath(player, Time::now());
}

void ClassSelectionSystem::onPlayerSpawn(IPlayer &player)
{
    m_classSelectionService.handleSpawn(player);
}

void ClassSelectionSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_classSelectionService.resetPlayer(player.getID());
}
