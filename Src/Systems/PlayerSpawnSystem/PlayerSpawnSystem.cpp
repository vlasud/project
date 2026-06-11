#include "Systems/PlayerSpawnSystem/PlayerSpawnSystem.h"

#include "Services/Core/ClassSelectionService/ClassSelectionService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"

PlayerSpawnSystem::PlayerSpawnSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_spawnService(serviceRegister.getService<PlayerSpawnService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);

    m_spawnService.initialize(&serviceRegister.getService<PlayerLocationService>());

    // Любой вход в класс-селекшн (коннект, F4+смерть) сводится к одному:
    // немедленно заспавнить игрока в точку из источника правды. Что делать
    // дальше (спектейт авторизации, экипировка) — системы решают на событии
    // спавна, о класс-селекшне никто из них не знает.
    serviceRegister.getService<ClassSelectionService>().setEntryHandler(
        [this](IPlayer &player) { m_spawnService.respawn(player); });
}

void PlayerSpawnSystem::onPlayerConnect(IPlayer &player)
{
    m_spawnService.handleConnect(player);
}

void PlayerSpawnSystem::onPlayerSpawn(IPlayer &player)
{
    m_spawnService.handleSpawn(player);
}

void PlayerSpawnSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_spawnService.resetPlayer(player.getID());
}
