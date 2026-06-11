#pragma once

#include "Services/Core/PlayerKeyService/PlayerKeyService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Единственный подписчик onPlayerKeyStateChange: маршрутизирует смены клавиш в
// PlayerKeyService и чистит per-player состояние при отключении.
class PlayerKeySystem : public BaseSystem, public PlayerChangeEventHandler, public PlayerConnectEventHandler
{
  public:
    PlayerKeySystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerKeyStateChange(IPlayer &player, uint32_t newKeys, uint32_t oldKeys) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerKeyService &m_keyService;
};
