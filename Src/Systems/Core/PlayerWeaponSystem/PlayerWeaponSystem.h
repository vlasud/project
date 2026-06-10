#pragma once

#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Привод сервиса оружия: каждый выстрел (bullet sync) проверяется на владение и
// списывает патрон, оружие в руках сверяется с инвентарём на каждом апдейте,
// нарушения уходят в журнал античита.
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
    bool handleShot(IPlayer &player, const PlayerBulletData &bulletData);

    PlayerWeaponService &m_weaponService;
    AntiCheatService &m_antiCheatService;
};
