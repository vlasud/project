#pragma once

#include "Services/Core/PlayerSkinService/PlayerSkinService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник PlayerSkinService: сброс скина к дефолту при подключении/выходе.
// На спавне ничего делать не нужно — PlayerSpawnService кладёт скин из сервиса
// в spawn-инфо, и клиент спавнится сразу правильным.
class PlayerSkinSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    PlayerSkinSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerSkinService &m_skinService;
};
