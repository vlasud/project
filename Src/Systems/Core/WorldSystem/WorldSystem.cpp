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

void WorldSystem::initialize(IComponentList * /*components*/)
{
    // Динамическая смена дня и ночи: игровое время = реальные часы машины
    // сервера (полный цикл за 24 реальных часа), обновление на границе каждой
    // реальной минуты. Именно в initialize: конструкторная фаза ещё без
    // ITimersComponent — setTimeout дропнулся бы и время застыло на стартовом.
    // HUD-часы при этом НЕ включаем: с ними клиент сам тикает время в игровом
    // темпе (1 мин/сек) и между синками расходился бы с сервером.
    m_worldService.setRealTimeSync(true);
}

void WorldSystem::onPlayerSpawn(IPlayer &player)
{
    m_worldService.handleSpawn(player);
}

void WorldSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_worldService.resetPlayer(player.getID());
}
