#pragma once

#include "../../Services/AntiCheatService/AntiCheatService.h"
#include "../../Services/PlayerAnimationService/PlayerAnimationService.h"
#include "../BaseSystem.h"
#include "player.hpp"

// Связывает сервис анимаций с событиями игрока: на каждом апдейте сверяет
// заявленную клиентом анимацию с выставленной сервером, на отключении — сбрасывает
// состояние слота.
class PlayerAnimationSystem : public BaseSystem, public PlayerUpdateEventHandler, public PlayerConnectEventHandler
{
  public:
    PlayerAnimationSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerAnimationService &m_animationService;
    AntiCheatService &m_antiCheatService;
};
