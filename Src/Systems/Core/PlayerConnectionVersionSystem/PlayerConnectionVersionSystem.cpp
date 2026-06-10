#include "Systems/Core/PlayerConnectionVersionSystem/PlayerConnectionVersionSystem.h"

PlayerConnectionVersionSystem::PlayerConnectionVersionSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_playerConnectionVersionService(serviceRegister.getService<PlayerConnectionVersionService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void PlayerConnectionVersionSystem::onPlayerConnect(IPlayer &player)
{
    m_playerConnectionVersionService.changeVersion(player.getID());
}
