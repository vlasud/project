#include "Systems/Core/ScreenTimerSystem/ScreenTimerSystem.h"

#include "Services/Core/TextDrawService/TextDrawService.h"

ScreenTimerSystem::ScreenTimerSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_screenTimer(serviceRegister.getService<ScreenTimerService>())
{
    m_screenTimer.initialize(&serviceRegister.getService<TextDrawService>());
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void ScreenTimerSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_screenTimer.resetPlayer(player.getID());
}
