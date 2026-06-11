#pragma once

#include "Services/Core/CameraService/CameraService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник CameraService: связывает с ядром/таймерами и чистит проигрывание
// при отключении игрока.
class CameraSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    CameraSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    CameraService &m_cameraService;
};
