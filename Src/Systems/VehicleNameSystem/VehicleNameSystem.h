#pragma once

#include "Services/Core/GameTextService/GameTextService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Попап названия машины (SA-стиль 7, правый нижний угол) при посадке за руль.
// Только водителю (PlayerState_Driver) — пассажирам не показываем. Косметика:
// один GameText через GameTextService, серверная логика от него не зависит.
class VehicleNameSystem : public BaseSystem, public PlayerChangeEventHandler
{
  public:
    VehicleNameSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerStateChange(IPlayer &player, PlayerState newState, PlayerState oldState) override;

  private:
    VehicleService &m_vehicleService;
    GameTextService &m_gameText;
};
