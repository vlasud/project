#pragma once

#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник ScreenNoticeService: передаёт сервису зависимости (TextDrawService,
// TimerService) и чистит per-player состояние (единый textdraw + pending) на
// дисконнекте.
class ScreenNoticeSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    ScreenNoticeSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    ScreenNoticeService &m_screenNotice;
};
