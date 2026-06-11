#include "Systems/Core/PlayerMoneySystem/PlayerMoneySystem.h"

PlayerMoneySystem::PlayerMoneySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void PlayerMoneySystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_moneyService.reset(player.getID());
}
