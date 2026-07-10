#pragma once

#include "Services/Core/ScreenTimerService/ScreenTimerService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник ScreenTimerService: передаёт сервису зависимость (TextDrawService) и
// чистит per-player состояние (единый textdraw-бар) на дисконнекте.
class ScreenTimerSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    ScreenTimerSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    ScreenTimerService &m_screenTimer;
};
