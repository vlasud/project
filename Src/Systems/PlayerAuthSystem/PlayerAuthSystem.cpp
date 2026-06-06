#include "PlayerAuthSystem.h"

PlayerAuthSystem::PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_playerAuthService(serviceRegister.getService<PlayerAuthService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void PlayerAuthSystem::initialize()
{
}

void PlayerAuthSystem::reset()
{
}

void PlayerAuthSystem::onPlayerConnect(IPlayer &player)
{
    if (m_playerAuthService.isPlayerAuthenticated(player.getID()))
    {
        player.kick();
        return;
    }

    player.sendClientMessage(Colour::White(), "Hello, world");
}

void PlayerAuthSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
}
