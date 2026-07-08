#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

class PlayerWeaponPersistSystem;

// Кэш загруженного из БД оружия аккаунта. Бизнес-фича персиста, НЕ Core:
// PlayerWeaponService остаётся чистым рантайм-состоянием без доступа к БД (см.
// Docs/Persistence.md); саму загрузку/сохранение делает PlayerWeaponPersistSystem.
// Оружие сбрасывается на КАЖДОМ спавне — точку РАЗОВОЙ выдачи на логин-спавне
// держит PlayerAuthSystem (m_pendingSpawnSetup), а areWeaponsApplied взводится
// ИМ ЖЕ после реального giveWeapon (см. ниже) — это отдельный от areWeaponsLoaded
// гейт для сохранения.
//
// Тайминг применения и гонка с загрузкой: сессия стартует и загрузка (async-
// select) уходит на воркер, а логин-спавн игрока — отдельный клиентский
// round-trip (finalize -> setSpectating(false) -> ожидание onPlayerSpawn). В
// подавляющем большинстве случаев БД успевает раньше. На случай обратного —
// спавн пришёл РАНЬШЕ загрузки — PlayerAuthSystem подписывается на
// subscribeWeaponsLoaded: наблюдатель применит кэш, как только загрузка
// догонит (сам PlayerAuthSystem решает, ждёт он этого события или нет, —
// сервис лишь оповещает КАЖДУЮ загрузку, включая штатный случай).
class PlayerWeaponPersistService final : public IService
{
    friend PlayerWeaponPersistSystem;

  public:
    // --- чтение кэша (для применения в PlayerAuthSystem) ---
    bool areWeaponsLoaded(int playerId) const;
    const std::vector<std::pair<std::uint8_t, int>> &cachedWeapons(int playerId) const;

    // Оружие реально уходит в PlayerWeaponService не в момент загрузки, а на
    // логин-спавне (инвентарь чистится на каждом спавне) — areWeaponsLoaded
    // говорит лишь «кэш из БД пришёл», а НЕ «giveWeapon уже прогнан».
    // markWeaponsApplied зовёт PlayerAuthSystem РОВНО там, где giveWeapon
    // реально выполнился (m_pendingSpawnSetup и late-observer); save-guard в
    // PlayerWeaponPersistSystem обязан ждать areWeaponsApplied, иначе снимок
    // ещё не выданного (пустого рантайм-инвентаря) состояния затрёт БД.
    bool areWeaponsApplied(int playerId) const;
    void markWeaponsApplied(int playerId);

    // --- наблюдатель: загрузка завершилась (в т.ч. late — после уже
    // пройденного спавна). Зовётся ПОСЛЕ serial-guard в PlayerWeaponPersistSystem
    // — playerId гарантированно принадлежит текущей сессии слота.
    using WeaponsObserver = std::function<void(IPlayer &, const std::vector<std::pair<std::uint8_t, int>> &)>;
    void subscribeWeaponsLoaded(WeaponsObserver observer);

  private:
    // --- вызывается PlayerWeaponPersistSystem ---
    void loadWeapons(IPlayer &player, std::vector<std::pair<std::uint8_t, int>> weapons);
    void reset(int playerId);

    struct WeaponsCache
    {
        bool loaded = false;
        std::vector<std::pair<std::uint8_t, int>> weapons;
        bool applied = false; // giveWeapon реально прогнан по этому кэшу (спавн/late-observer)
    };

    std::array<WeaponsCache, MAX_PLAYERS> m_weapons;
    std::vector<WeaponsObserver> m_weaponsObservers;
};
