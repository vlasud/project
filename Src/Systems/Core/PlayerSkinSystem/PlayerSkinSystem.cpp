#include "Systems/Core/PlayerSkinSystem/PlayerSkinSystem.h"

PlayerSkinSystem::PlayerSkinSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_skinService(serviceRegister.getService<PlayerSkinService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);
}

void PlayerSkinSystem::onPlayerConnect(IPlayer &player)
{
    m_skinService.resetPlayer(player.getID());
}

void PlayerSkinSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_skinService.resetPlayer(player.getID());
}

void PlayerSkinSystem::onPlayerSpawn(IPlayer &player)
{
    // Временный скин (/askin) живёт только до респауна. Spawn-инфо уже строится
    // из БАЗЫ (PlayerSpawnService::applySpawnInfo до player.spawn()), поэтому
    // клиент к этому моменту в базовом скине — снимаем оверрайд, синхронизируя
    // live-состояние сервиса (setSkin(base) идемпотентен).
    m_skinService.clearTempSkin(player);
}
