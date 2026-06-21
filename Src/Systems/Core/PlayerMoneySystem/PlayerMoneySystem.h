#pragma once

#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Привод сервиса денег: на дисконнекте сбрасывает баланс слота, а на спавне ЗАНОВО
// отправляет серверный баланс клиенту — иначе списанные GTA-клиентом при смерти $100
// (госпиталь single-player GTA:SA) остаются на HUD до следующей денежной операции.
class PlayerMoneySystem : public BaseSystem, public PlayerConnectEventHandler, public PlayerSpawnEventHandler
{
  public:
    PlayerMoneySystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerMoneyService &m_moneyService;
    AntiCheatService &m_antiCheatService;
};
