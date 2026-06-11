#pragma once

#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerVelocityService/PlayerVelocityService.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Привод сервиса оружия: каждый выстрел (bullet sync) проверяется на владение,
// списывает патрон и валидируется (NaN, origin spoof, дальность, rapid fire,
// silent aim) против серверных фактов — принятой позиции стрелка и серверной
// позиции/скорости цели. Оружие в руках сверяется с инвентарём на каждом
// апдейте, нарушения уходят в журнал античита, невалидный выстрел дропается
// (health-система его не зарегистрирует — give-damage по нему не пройдёт).
class PlayerWeaponSystem : public BaseSystem,
                           public PlayerShotEventHandler,
                           public PlayerUpdateEventHandler,
                           public PlayerSpawnEventHandler,
                           public PlayerConnectEventHandler
{
  public:
    PlayerWeaponSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerShotMissed(IPlayer &player, const PlayerBulletData &bulletData) override;
    bool onPlayerShotPlayer(IPlayer &player, IPlayer &target, const PlayerBulletData &bulletData) override;
    bool onPlayerShotVehicle(IPlayer &player, IVehicle &target, const PlayerBulletData &bulletData) override;
    bool onPlayerShotObject(IPlayer &player, IObject &target, const PlayerBulletData &bulletData) override;
    bool onPlayerShotPlayerObject(IPlayer &player, IPlayerObject &target, const PlayerBulletData &bulletData) override;

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    // targetPos — серверная позиция цели (null — промах/нет цели),
    // targetPlayer — цель-игрок (для допусков по скорости и silent aim).
    bool handleShot(IPlayer &player, const PlayerBulletData &bulletData, const Vector3 *targetPos,
                    const IPlayer *targetPlayer);

    PlayerWeaponService &m_weaponService;
    AntiCheatService &m_antiCheatService;
    PlayerLocationService &m_locationService;
    PlayerVelocityService &m_velocityService;
};
