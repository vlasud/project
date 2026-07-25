#include "Systems/Core/PlayerWeaponSystem/PlayerWeaponSystem.h"

#include <Server/Components/Vehicles/vehicles.hpp>
#include <chrono>

PlayerWeaponSystem::PlayerWeaponSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_weaponService(serviceRegister.getService<PlayerWeaponService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_velocityService(serviceRegister.getService<PlayerVelocityService>()),
      m_weaponProficiencyService(serviceRegister.getService<WeaponProficiencyService>())
{
    core.getPlayers().getPlayerShotDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

namespace
{
AntiCheatService::ViolationType toViolation(PlayerWeaponService::ShotFlag flag)
{
    switch (flag)
    {
    case PlayerWeaponService::ShotFlag::ShotHack:
        return AntiCheatService::ViolationType::ShotHack;
    case PlayerWeaponService::ShotFlag::RapidFire:
        return AntiCheatService::ViolationType::RapidFire;
    case PlayerWeaponService::ShotFlag::SilentAim:
        return AntiCheatService::ViolationType::SilentAim;
    default:
        return AntiCheatService::ViolationType::WeaponHack;
    }
}
} // namespace

bool PlayerWeaponSystem::handleShot(IPlayer &player, const PlayerBulletData &bulletData, const Vector3 *targetPos,
                                    const IPlayer *targetPlayer)
{
    const TimePoint now = std::chrono::steady_clock::now();

    PlayerWeaponService::ShotContext ctx;
    ctx.shooterPos = m_locationService.getPosition(player.getID());
    ctx.targetPos = targetPos;
    if (targetPlayer)
    {
        // Вектор, а не модуль: сервису нужно упреждение серверной позиции цели.
        ctx.targetVelocity = m_velocityService.getVelocity(targetPlayer->getID());
        // Авто-прицел драйв-бая легально стреляет под углом к камере.
        ctx.checkSilentAim = player.getState() == PlayerState_OnFoot;
    }

    PlayerWeaponService::ShotOutcome outcome = m_weaponService.onShot(player, bulletData, ctx, now);
    if (outcome.flag != PlayerWeaponService::ShotFlag::None)
    {
        m_antiCheatService.record(player.getID(), toViolation(outcome.flag), std::move(outcome.detail), now);
    }
    if (!outcome.drop)
    {
        // Прогрессия владения — ТОЛЬКО по серверно-валидному выстрелу: фейковые/
        // rapid-fire/ammo-hack выстрелы дропнуты выше и сюда не доходят, фарма
        // скилла фейковыми RPC нет. Оружие — из серверного bulletData.weapon.
        // registerShot — O(1), без аллокаций/БД (hot path, каждая пуля).
        m_weaponProficiencyService.registerShot(player.getID(), bulletData.weapon);
    }
    return !outcome.drop; // фейковый выстрел дальше по конвейеру не идёт
}

bool PlayerWeaponSystem::onPlayerShotMissed(IPlayer &player, const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData, nullptr, nullptr);
}

bool PlayerWeaponSystem::onPlayerShotPlayer(IPlayer &player, IPlayer &target, const PlayerBulletData &bulletData)
{
    // Серверная позиция цели — принятая LocationService, не сырая клиентская.
    const Vector3 targetPos = m_locationService.getPosition(target.getID());
    return handleShot(player, bulletData, &targetPos, &target);
}

bool PlayerWeaponSystem::onPlayerShotVehicle(IPlayer &player, IVehicle &target, const PlayerBulletData &bulletData)
{
    const Vector3 targetPos = target.getPosition();
    return handleShot(player, bulletData, &targetPos, nullptr);
}

bool PlayerWeaponSystem::onPlayerShotObject(IPlayer &player, IObject &target, const PlayerBulletData &bulletData)
{
    // Дальность по объектам не сверяем: позиция объекта — центр модели, а модели
    // бывают огромными (мост) — попадание в край дальше «дальности до центра».
    // Урон по объектам не идёт, абуза нет.
    return handleShot(player, bulletData, nullptr, nullptr);
}

bool PlayerWeaponSystem::onPlayerShotPlayerObject(IPlayer &player, IPlayerObject &target,
                                                  const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData, nullptr, nullptr);
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
