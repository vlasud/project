#include "Systems/Core/PlayerKeySystem/PlayerKeySystem.h"

PlayerKeySystem::PlayerKeySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_keyService(serviceRegister.getService<PlayerKeyService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
    listen(core.getPlayers().getPlayerChangeDispatcher(), this);
}

void PlayerKeySystem::onPlayerKeyStateChange(IPlayer &player, uint32_t newKeys, uint32_t oldKeys)
{
    m_keyService.handleKeyStateChange(player, newKeys, oldKeys, Time::now());
}

void PlayerKeySystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_keyService.resetPlayer(player.getID());
}
