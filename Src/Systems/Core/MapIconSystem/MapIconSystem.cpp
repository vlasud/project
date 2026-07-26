#include "Systems/Core/MapIconSystem/MapIconSystem.h"

#include "Services/Core/StreamerService/StreamerService.h"

MapIconSystem::MapIconSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_mapIconService(serviceRegister.getService<MapIconService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);

    m_mapIconService.initialize(&serviceRegister.getService<StreamerService>());
}

void MapIconSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_mapIconService.resetPlayer(player.getID());
}
