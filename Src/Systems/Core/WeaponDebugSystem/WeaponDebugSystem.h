#pragma once

#include "Macro.h"
#include "Services/Core/GameTextService/GameTextService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <cstdint>

// Дев-тулза замера темпа стрельбы — для настройки таблицы минимальных
// интервалов анти-rapid-fire в PlayerWeaponService.
//
//   /rof — вкл: на каждом выстреле гейм-текст с интервалом от прошлого выстрела,
//          мин/средним по оружию и табличным порогом;
//          выкл: сводка по каждому стрелявшему оружию в чат.
//
// ВАЖНО: система регистрируется РАНЬШЕ PlayerWeaponSystem — замер видит сырой
// клиентский темп, включая выстрелы, которые валидатор дальше дропнет.
class WeaponDebugSystem : public BaseSystem, public PlayerShotEventHandler, public PlayerConnectEventHandler
{
  public:
    WeaponDebugSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerShotMissed(IPlayer &player, const PlayerBulletData &bulletData) override;
    bool onPlayerShotPlayer(IPlayer &player, IPlayer &target, const PlayerBulletData &bulletData) override;
    bool onPlayerShotVehicle(IPlayer &player, IVehicle &target, const PlayerBulletData &bulletData) override;
    bool onPlayerShotObject(IPlayer &player, IObject &target, const PlayerBulletData &bulletData) override;
    bool onPlayerShotPlayerObject(IPlayer &player, IPlayerObject &target, const PlayerBulletData &bulletData) override;

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    static constexpr std::size_t MAX_WEAPON_ID = 47; // стреляющие — 22..38, с запасом

    struct WeaponStat
    {
        std::uint32_t count = 0; // замеренных интервалов (выстрелов минус первые в очередях)
        std::uint32_t minMs = 0;
        std::uint64_t sumMs = 0;
    };

    struct DebugState
    {
        bool enabled = false;
        bool hasLast = false;
        std::uint8_t lastWeapon = 0;
        TimePoint lastShot;
        std::array<WeaponStat, MAX_WEAPON_ID> stats{};
    };

    bool handleShot(IPlayer &player, const PlayerBulletData &bulletData);
    void toggle(IPlayer &player);
    void showSummary(IPlayer &player, const DebugState &state);

    GameTextService &m_gameTextService;
    std::array<DebugState, MAX_PLAYERS> m_state;
};
