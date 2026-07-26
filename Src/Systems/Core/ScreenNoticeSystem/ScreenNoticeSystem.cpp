#include "Systems/Core/ScreenNoticeSystem/ScreenNoticeSystem.h"

#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Services/Core/TimerService/TimerService.h"

ScreenNoticeSystem::ScreenNoticeSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_screenNotice(serviceRegister.getService<ScreenNoticeService>())
{
    m_screenNotice.initialize(&serviceRegister.getService<TextDrawService>(), &serviceRegister.getService<TimerService>());
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

void ScreenNoticeSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_screenNotice.resetPlayer(player.getID());
}
