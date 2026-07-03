#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <Server/Components/Classes/classes.hpp>
#include <array>

class PlayerSpawnSystem;
class PlayerLocationService;
class PlayerSkinService;

// Точка спавна игрока: где он появится. Скин в точку не входит — его единый
// источник правды PlayerSkinService (спавн берёт текущее значение оттуда).
struct SpawnPoint
{
    Vector3 position{2690.5237f, -2479.6560f, 13.6509f}; // дефолт — порт (совпадает со STATION в SpawnChoiceSystem)
    float angle = 89.2331f;
    unsigned interior = 0;
    int virtualWorld = 0;
};

// Единственный источник правды о спавне игрока.
//
// setSpawn() задаёт точку, в которой игрок появится при ЛЮБОМ следующем спавне:
// после смерти, после respawn(), после серверного player.spawn(). Точка сразу
// прокидывается в class-данные клиента (setSpawnInfo), поэтому даже нативный
// респаун после смерти приходит ровно туда — без телепортов вдогонку. Интерьер
// и виртуальный мир применяются системой на событии спавна.
//
//   m_spawn.setSpawn(player, {hospitalPos, 90.0f, 0, 0, skinId});
//   m_spawn.respawn(player); // принудительный респаун прямо сейчас
//
// Никакая другая система не должна телепортить/менять скин на спавне — только
// через setSpawn, иначе источников правды снова станет два.
class PlayerSpawnService final : public IService
{
    friend PlayerSpawnSystem;

  public:
    // Задать точку спавна игрока (валидируется и применяется ко всем
    // последующим спавнам до переустановки или выхода игрока).
    void setSpawn(IPlayer &player, const SpawnPoint &point);
    // Текущая точка спавна (заданная или дефолтная).
    const SpawnPoint &getSpawn(int playerId) const;

    // Дефолт для игроков, которым setSpawn не вызывали (новые подключения).
    void setDefaultSpawn(const SpawnPoint &point);

    // Принудительный респаун прямо сейчас — в точку из getSpawn().
    void respawn(IPlayer &player);

  private:
    struct Slot
    {
        bool customized = false;
        SpawnPoint point{};
    };

    // Вызываются PlayerSpawnSystem.
    void initialize(PlayerLocationService *location, PlayerSkinService *skins);
    void handleConnect(IPlayer &player); // прокинуть дефолт в class-данные
    void handleSpawn(IPlayer &player);   // интерьер/мир после фактического спавна
    void resetPlayer(int playerId);

    void applySpawnInfo(IPlayer &player); // точка -> class-данные клиента
    static SpawnPoint sanitize(SpawnPoint point);

    PlayerLocationService *m_location = nullptr;
    PlayerSkinService *m_skins = nullptr;
    SpawnPoint m_defaultSpawn{};
    std::array<Slot, MAX_PLAYERS> m_slots;
};
