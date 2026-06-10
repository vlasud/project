#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <string>

// Сервис оружия — серверный инвентарь как единственный источник истины о том,
// какое оружие и сколько патронов есть у игрока.
//
// Клиент может «нарисовать» себе любое оружие локально (weapon hack) — оружие в
// руках приходит в каждом sync. Поэтому:
//  * выдача/изъятие — ТОЛЬКО через giveWeapon/removeWeapon этого сервиса;
//  * на каждом апдейте оружие в руках клиента сверяется с инвентарём: чужое —
//    снимается + нарушение;
//  * каждый выстрел (bullet sync) проверяется на владение и списывает патрон;
//    стрельба при серверном нуле патронов (с запасом на дрейф) — ammo hack:
//    патроны обнуляются принудительно + нарушение.
//
// На спавне инвентарь чистится (GTA теряет оружие на смерти) — игровая логика
// перевыдаёт через сервис.
class PlayerWeaponService final : public IService
{
  public:
    // --- источник истины ---
    bool hasWeapon(int playerId, std::uint8_t weaponId) const;
    int getAmmo(int playerId, std::uint8_t weaponId) const; // -1 — оружия нет
    std::uint8_t getArmedWeapon(int playerId) const;        // принятое оружие в руках

    // --- серверные операции ---
    void giveWeapon(IPlayer &player, std::uint8_t weaponId, std::uint32_t ammo);
    void removeWeapon(IPlayer &player, std::uint8_t weaponId);
    void resetWeapons(IPlayer &player);
    void setAmmo(IPlayer &player, std::uint8_t weaponId, std::uint32_t ammo);

    struct Outcome
    {
        bool weaponHack = false;
        std::string detail;
    };

    // --- вызывается PlayerWeaponSystem ---
    Outcome onShot(IPlayer &player, std::uint8_t weaponId, TimePoint now); // из bullet sync
    Outcome verifyArmed(IPlayer &player, TimePoint now);                   // каждый апдейт
    void onSpawn(IPlayer &player);
    void reset(int playerId);

  private:
    struct Slot
    {
        std::uint8_t id = 0;
        std::int32_t ammo = 0; // signed: уходит в минус до порога долга (дрейф)
    };

    struct State
    {
        std::array<Slot, MAX_WEAPON_SLOTS> slots;
        std::uint8_t armed = 0; // принятое оружие в руках
        TimePoint lastChange;   // грейс синхронизации после выдачи/изъятия
        TimePoint lastFlag;     // rate limit повторных нарушений
    };

    Slot *findWeapon(State &st, std::uint8_t weaponId);
    const Slot *findWeapon(const State &st, std::uint8_t weaponId) const;

    std::array<State, MAX_PLAYERS> m_state;
};
