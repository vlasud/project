#include "Systems/Core/StreamerSystem/StreamerSystem.h"

#include <chrono>

namespace
{
constexpr std::chrono::milliseconds PICKUP_SWEEP_INTERVAL{1000};
}

StreamerSystem::StreamerSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_gridService(serviceRegister.getService<GridService>()),
      m_streamerService(serviceRegister.getService<StreamerService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>())
{
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getEventDispatcher().addEventHandler(this);
}

void StreamerSystem::initialize(IComponentList *components)
{
    m_streamerService.initialize(m_core, m_gridService, components->queryComponent<IPickupsComponent>());
}

bool StreamerSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    // Позиция и vw — принятые сервером (PlayerLocationService), не клиентские.
    const int playerId = player.getID();
    m_streamerService.streamPlayer(player, m_locationService.getPosition(playerId),
                                   m_locationService.getVirtualWorld(playerId), now);
    return true;
}

void StreamerSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_streamerService.resetPlayer(player.getID());
}

void StreamerSystem::onTick(Microseconds elapsed, TimePoint now)
{
    if (now < m_nextPickupSweep)
        return;
    m_nextPickupSweep = now + PICKUP_SWEEP_INTERVAL;
    m_streamerService.sweepPickups(now);
}
