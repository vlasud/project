#pragma once

#include "../../Services/AntiCheatService/AntiCheatService.h"
#include "../../Services/PlayerLocationService/PlayerLocationService.h"
#include "../BaseSystem.h"
#include "player.hpp"

// Связывает сервис местонахождения с событиями: на каждом апдейте сверяет
// заявленную клиентом позицию с принятой (детект телепорт-хака), спавн и смена
// интерьера выдают легальные грейсы, нарушения уходят в журнал античита.
class PlayerLocationSystem : public BaseSystem,
                             public PlayerUpdateEventHandler,
                             public PlayerSpawnEventHandler,
                             public PlayerChangeEventHandler,
                             public PlayerConnectEventHandler
{
  public:
    PlayerLocationSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerInteriorChange(IPlayer &player, unsigned newInterior, unsigned oldInterior) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerLocationService &m_locationService;
    AntiCheatService &m_antiCheatService;
};
