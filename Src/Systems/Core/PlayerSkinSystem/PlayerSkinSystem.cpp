#include "Systems/Core/PlayerSkinSystem/PlayerSkinSystem.h"

PlayerSkinSystem::PlayerSkinSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_skinService(serviceRegister.getService<PlayerSkinService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void PlayerSkinSystem::onPlayerConnect(IPlayer &player)
{
    m_skinService.resetPlayer(player.getID());
}

void PlayerSkinSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_skinService.resetPlayer(player.getID());
}
