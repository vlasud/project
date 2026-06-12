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

    // Стандартные входы в интерьеры GTA SA выключены: телепорт жёлтым enex —
    // клиентский (мимо LocationService), а все нужные здания открываются
    // нашими пикапами (базы фракций, мэрии и т.п.).
    m_worldService.setInteriorEnterExits(false);
}

void WorldSystem::onPlayerSpawn(IPlayer &player)
{
    m_worldService.handleSpawn(player);
}

void WorldSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_worldService.resetPlayer(player.getID());
}
