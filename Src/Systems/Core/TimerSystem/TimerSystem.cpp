#include "Systems/Core/TimerSystem/TimerSystem.h"
#include "Log/LogManager.h"

TimerSystem::TimerSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_timerService(serviceRegister.getService<TimerService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

void TimerSystem::initialize(IComponentList *components)
{
    ITimersComponent *timers = components->queryComponent<ITimersComponent>();
    if (!timers)
    {
        LogManager::log(Error, "TimerSystem: ITimersComponent is missing, timers are disabled");
    }
    m_timerService.initialize(&m_core, timers);
}

void TimerSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_timerService.resetPlayer(player.getID());
}
