#include "Systems/Core/PlayerWeaponSystem/PlayerWeaponSystem.h"

#include <chrono>

PlayerWeaponSystem::PlayerWeaponSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_weaponService(serviceRegister.getService<PlayerWeaponService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>())
{
    core.getPlayers().getPlayerShotDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

bool PlayerWeaponSystem::handleShot(IPlayer &player, const PlayerBulletData &bulletData)
{
    const TimePoint now = std::chrono::steady_clock::now();
    PlayerWeaponService::Outcome outcome = m_weaponService.onShot(player, bulletData.weapon, now);
    if (outcome.weaponHack)
    {
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::WeaponHack,
                                  std::move(outcome.detail), now);
        return false; // фейковый выстрел дальше по конвейеру не идёт
    }
    return true;
}

bool PlayerWeaponSystem::onPlayerShotMissed(IPlayer &player, const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData);
}

bool PlayerWeaponSystem::onPlayerShotPlayer(IPlayer &player, IPlayer &target, const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData);
}

bool PlayerWeaponSystem::onPlayerShotVehicle(IPlayer &player, IVehicle &target, const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData);
}

bool PlayerWeaponSystem::onPlayerShotObject(IPlayer &player, IObject &target, const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData);
}

bool PlayerWeaponSystem::onPlayerShotPlayerObject(IPlayer &player, IPlayerObject &target,
                                                  const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData);
}

bool PlayerWeaponSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    PlayerWeaponService::Outcome outcome = m_weaponService.verifyArmed(player, now);
    if (outcome.weaponHack)
    {
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::WeaponHack,
                                  std::move(outcome.detail), now);
    }
    return true;
}

void PlayerWeaponSystem::onPlayerSpawn(IPlayer &player)
{
    m_weaponService.onSpawn(player);
}

void PlayerWeaponSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_weaponService.reset(player.getID());
}
