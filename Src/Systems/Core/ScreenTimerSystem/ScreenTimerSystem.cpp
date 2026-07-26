#include "Systems/Core/ScreenTimerSystem/ScreenTimerSystem.h"

#include "Services/Core/TextDrawService/TextDrawService.h"

ScreenTimerSystem::ScreenTimerSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_screenTimer(serviceRegister.getService<ScreenTimerService>())
{
    m_screenTimer.initialize(&serviceRegister.getService<TextDrawService>());
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

void ScreenTimerSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_screenTimer.resetPlayer(player.getID());
}
