#pragma once

#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Попап названия машины (единый экранный textdraw ScreenNoticeService, белый
// цвет) при посадке за руль. Только водителю (PlayerState_Driver) — пассажирам
// не показываем. Косметика: свой textdraw-попап (не native GameText — см.
// Docs/ScreenNotice.md), серверная логика от него не зависит.
class VehicleNameSystem : public BaseSystem, public PlayerChangeEventHandler
{
  public:
    VehicleNameSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerStateChange(IPlayer &player, PlayerState newState, PlayerState oldState) override;

  private:
    VehicleService &m_vehicleService;
    ScreenNoticeService &m_screenNotice;
};
