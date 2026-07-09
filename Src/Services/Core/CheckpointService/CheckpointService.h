#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Server/Components/Checkpoints/checkpoints.hpp"
#include "types.hpp"
#include <array>
#include <functional>
#include <unordered_map>

class CheckpointSystem;
class PlayerLocationService;
class AntiCheatService;

// Сервис чекпоинтов. Клиент умеет показывать только ОДИН обычный чекпоинт,
// поэтому сервис делит их на два уровня:
//
//  * Глобальные — мировые точки (входы, работы). Каждому игроку автоматически
//    показывается ближайший в радиусе стрима:
//        int id = m_checkpoints.add({x, y, z}, 3.0f, [](IPlayer &p) { ... });
//        m_checkpoints.remove(id); // можно из обработчика
//
//  * Персональный — приоритетнее глобальных (маркер задания); пока установлен,
//    глобальные игроку не показываются:
//        m_checkpoints.setForPlayer(player, pos, 3.0f, onEnter, onLeave);
//        m_checkpoints.clearForPlayer(player);
//
//  * Гоночный — отдельный клиентский слот, на глобальные не влияет:
//        m_checkpoints.setRaceForPlayer(player, RaceCheckpointType::RACE_NORMAL, pos, nextPos, 5.0f, onEnter);
//
// Валидация входа: клиент может прислать enter-событие в любой момент — вход
// засчитывается только если принятая сервером позиция внутри радиуса (с
// допуском на лаг), иначе фиксируется CheckpointHack в журнале античита.
class CheckpointService final : public IService
{
    friend CheckpointSystem;

  public:
    using EnterHandler = std::function<void(IPlayer &)>;
    using LeaveHandler = std::function<void(IPlayer &)>;

    static constexpr float MIN_RADIUS = 0.5f;
    static constexpr float MAX_RADIUS = 100.0f;
    static constexpr float STREAM_DISTANCE = 300.0f; // радиус показа глобальных
    static constexpr float ENTER_SLACK = 15.0f;      // допуск дистанции входа (лаг)

    // --- глобальные ---
    // Возвращает id чекпоинта.
    int add(const Vector3 &position, float radius, EnterHandler onEnter, LeaveHandler onLeave = nullptr);
    void remove(int checkpointId);
    bool exists(int checkpointId) const;
    // Обновить позицию/радиус глобального чекпоинта (перепоказ — ближайшим
    // проходом стрима, до 500 мс).
    bool update(int checkpointId, const Vector3 &position, float radius);

    // --- персональный ---
    void setForPlayer(IPlayer &player, const Vector3 &position, float radius, EnterHandler onEnter = nullptr,
                      LeaveHandler onLeave = nullptr);
    void clearForPlayer(IPlayer &player);
    bool hasPersonal(int playerId) const;

    // --- гоночный (персональный) ---
    void setRaceForPlayer(IPlayer &player, RaceCheckpointType type, const Vector3 &position,
                          const Vector3 &nextPosition, float radius, EnterHandler onEnter = nullptr,
                          LeaveHandler onLeave = nullptr);
    void clearRaceForPlayer(IPlayer &player);

  private:
    struct Def
    {
        Vector3 position{};
        float radius = 0.0f;
        EnterHandler onEnter;
        LeaveHandler onLeave;
    };

    struct Slot
    {
        // персональный чекпоинт
        bool personal = false;
        Vector3 personalPosition{};
        float personalRadius = 0.0f;
        EnterHandler personalEnter;
        LeaveHandler personalLeave;
        // гоночный
        bool race = false;
        Vector3 racePosition{};
        float raceRadius = 0.0f;
        EnterHandler raceEnter;
        LeaveHandler raceLeave;
        // стриминг глобальных
        int shownDef = -1; // какой глобальный показан (-1 — никакой)
        TimePoint nextStreamAt{};
        // отложенный показ: Disable и Set должны попасть в разные кадры клиента,
        // иначе маркер не пересоздаётся и размер/высота остаются старыми
        bool pendingShow = false;
        Vector3 pendingPosition{};
        float pendingRadius = 0.0f;
        TimePoint pendingAt{};
        // дебаунс входов: осцилляция позиции на границе не должна спамить обработчики
        TimePoint lastEnterAt{};
        TimePoint lastRaceEnterAt{};
    };

    // Вызываются CheckpointSystem.
    void initialize(PlayerLocationService *location, AntiCheatService *antiCheat);
    void streamPlayer(IPlayer &player, const Vector3 &position, TimePoint now); // троттлится внутри
    void handleEnter(IPlayer &player, TimePoint now);
    void handleLeave(IPlayer &player);
    void handleRaceEnter(IPlayer &player, TimePoint now);
    void handleRaceLeave(IPlayer &player);
    void resetPlayer(int playerId);

    void showCheckpoint(IPlayer &player, const Vector3 &position, float radius);
    void hideCheckpoint(IPlayer &player);
    // Принятая позиция игрока внутри radius + ENTER_SLACK; иначе CheckpointHack.
    bool validateInside(IPlayer &player, const Vector3 &position, float radius, TimePoint now);
    static float clampRadius(float radius);

    PlayerLocationService *m_location = nullptr;
    AntiCheatService *m_antiCheat = nullptr;

    std::unordered_map<int, Def> m_defs;
    int m_nextId = 1;
    std::array<Slot, MAX_PLAYERS> m_slots;
};
