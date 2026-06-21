#pragma once

#include "Services/Core/PlayerSkinService/PlayerSkinService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник PlayerSkinService: сброс скина к дефолту при подключении/выходе и
// снятие временного оверрайда на спавне.
//
// На спавне СКИН не меняем напрямую: PlayerSpawnService кладёт БАЗУ из сервиса в
// spawn-инфо, и клиент спавнится сразу в ней. Снимаем лишь временный оверрайд —
// он живёт только до ближайшего респауна; clearTempSkin приводит live-состояние
// сервиса в соответствие с уже применённой базой.
class PlayerSkinSystem : public BaseSystem, public PlayerConnectEventHandler, public PlayerSpawnEventHandler
{
  public:
    PlayerSkinSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;
    void onPlayerSpawn(IPlayer &player) override;

  private:
    PlayerSkinService &m_skinService;
};
