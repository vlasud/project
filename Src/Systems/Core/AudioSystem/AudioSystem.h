#pragma once

#include "Services/Core/AudioService/AudioService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник AudioService: связывает с ядром и чистит per-player состояние
// потоков при отключении.
class AudioSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    AudioSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    AudioService &m_audioService;
};
