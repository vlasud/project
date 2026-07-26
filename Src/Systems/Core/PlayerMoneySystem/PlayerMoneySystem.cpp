#include "Systems/Core/PlayerMoneySystem/PlayerMoneySystem.h"

PlayerMoneySystem::PlayerMoneySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
    listen(core.getPlayers().getPlayerSpawnDispatcher(), this);
}

void PlayerMoneySystem::onPlayerSpawn(IPlayer &player)
{
    // Ре-синхрон HUD денег. GTA-клиент при смерти сам списывает $100 (госпиталь
    // single-player), сервер этого не делал — возвращаем HUD к серверному балансу.
    m_moneyService.syncToClient(player);
}

void PlayerMoneySystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_moneyService.reset(player.getID());
}
