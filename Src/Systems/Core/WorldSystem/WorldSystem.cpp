#include "Systems/Core/WorldSystem/WorldSystem.h"

WorldSystem::WorldSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_worldService(serviceRegister.getService<WorldService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);

    m_worldService.initialize(&core, &serviceRegister.getService<TimerService>());

    // Стант-бонусы выключены всегда: клиент начисляет за трюки деньги мимо
    // PlayerMoneyService — это дыра в источнике правды о деньгах (money-валидатор
    // считал бы их читерским ростом или, хуже, легализовал бы).
    m_worldService.setStuntBonuses(false);
}

void WorldSystem::onPlayerSpawn(IPlayer &player)
{
    m_worldService.handleSpawn(player);
}

void WorldSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_worldService.resetPlayer(player.getID());
}
