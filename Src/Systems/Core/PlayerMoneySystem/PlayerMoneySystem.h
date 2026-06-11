#pragma once

#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Привод сервиса денег: на каждом апдейте сверяет заявленный клиентом баланс с
// серверным, money hack пишет в журнал античита.
class PlayerMoneySystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    PlayerMoneySystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerMoneyService &m_moneyService;
    AntiCheatService &m_antiCheatService;
};
