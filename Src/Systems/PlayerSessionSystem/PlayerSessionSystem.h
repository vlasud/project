#pragma once

#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Привод сессий: закрывает сессию на дисконнекте (стреляют end-подписчики —
// бизнес-сервисы сохраняются) + дев-команда /session.
//
// ВАЖНО: система регистрируется раньше всех бизнес- и core-систем — конец
// сессии должен отстрелить ДО того, как чужие обработчики дисконнекта начнут
// чистить состояние игрока, иначе подписчики сохранят уже обнулённые данные.
class PlayerSessionSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    PlayerSessionSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerSessionService &m_sessionService;
};
