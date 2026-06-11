#include "Systems/Core/GangZoneSystem/GangZoneSystem.h"
#include "Log/LogManager.h"

GangZoneSystem::GangZoneSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_gangZoneService(serviceRegister.getService<GangZoneService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void GangZoneSystem::initialize(IComponentList *components)
{
    IGangZonesComponent *gangZones = components->queryComponent<IGangZonesComponent>();
    if (!gangZones)
    {
        LogManager::log(Error, "GangZoneSystem: IGangZonesComponent is missing, gang zones are disabled");
    }
    m_gangZoneService.initialize(&m_core, gangZones);
}

void GangZoneSystem::onPlayerConnect(IPlayer &player)
{
    m_gangZoneService.showAllForPlayer(player);
}
