#include "Systems/Core/CameraSystem/CameraSystem.h"

#include "Services/Core/PlayerConnectionVersionService/PlayerConnectionVersionService.h"

CameraSystem::CameraSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_cameraService(serviceRegister.getService<CameraService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    m_cameraService.initialize(&core, &serviceRegister.getService<TimerService>(),
                               &serviceRegister.getService<PlayerConnectionVersionService>());
}

void CameraSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_cameraService.resetPlayer(player.getID());
}
