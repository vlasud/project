#pragma once

#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Приветствие игрока при входе на сервер (бизнес-фича, не Core): попап через
// единый экранный textdraw ScreenNoticeService сразу после успешного логина.
class GreetingSystem : public BaseSystem
{
  public:
    GreetingSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    ScreenNoticeService &m_screenNotice;
};
