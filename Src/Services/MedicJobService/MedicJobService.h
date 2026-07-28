#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>
#include <functional>
#include <chrono>
#include <deque>
#include <vector>

class MedicJobSystem;

// Работа-врач (больница) — бизнес-фича (НЕ Core). Источник правды о:
//  * пер-player фазе смены (не работает / в очереди / машина выдана, идёт посадка /
//    на смене);
//  * занятости SPOT_COUNT точек спавна скорых;
//  * FIFO-очереди тех, кто ждёт свободную точку спавна;
//  * кулдауне лечения КАЖДОГО игрока (анти-абуз: одного и того же пациента нельзя
//    доить повторными вызовами).
//
// РАСХОЖДЕНИЕ с депо автобуса/развозчика: pre-stock машин НЕТ. Точка спавна пустует,
// пока очередь пуста; дошла очередь — на точке появляется ЛИЧНАЯ машина работника, и
// она же держит точку занятой, пока он не отъедет. Поэтому «посадка» здесь считается
// завершённой не по факту руля, а по факту ОТЪЕЗДА (иначе первый же работник
// заблокировал бы точку, сидя на ней).
//
// Кулдаун лечения принадлежит ПАЦИЕНТУ, а не врачу: иначе два врача лечили бы одного
// и того же по очереди. Живёт в памяти сессии — слот сбрасывается на её конце, чтобы
// переиспользованный playerId не унаследовал чужой кулдаун.
//
// Побочные эффекты (спавн/деспавн машин, скины, деньги, таймеры) — на приводе
// MedicJobSystem; сервис только считает состояние.
class MedicJobService final : public IService
{
    friend MedicJobSystem;

  public:
    MedicJobService();

    // Точки спавна скорых у больницы (замеры владельца, координаты — в приводе).
    static constexpr int SPOT_COUNT = 2;

    // Анти-абуз: одного пациента нельзя лечить чаще. Кулдаун общий на пациента —
    // сменой врача его не обойти.
    static constexpr std::chrono::minutes HEAL_COOLDOWN{10};

    enum class Phase
    {
        NotWorking, // не в смене
        Queued,     // устроен, ждёт свободную точку спавна в FIFO-очереди
        Boarding,   // машина заспавнена на точке: сесть и ОТЪЕХАТЬ, пока идёт окно
        Working     // на смене: машина своя, точка спавна освобождена
    };

    Phase phaseOf(int playerId) const;
    bool isWorking(int playerId) const; // phase != NotWorking
    int vehicleIdOf(int playerId) const;
    int queuePositionOf(int playerId) const; // 1-based место; 0 — не в очереди
    std::vector<int> queuedPlayers() const;  // снимок очереди для уведомлений о сдвиге

    // --- точки спавна ---
    int spotOf(int playerId) const;   // точка, которую держит игрок, или -1
    int holderOfSpot(int spot) const; // кто держит точку, или -1
    // Работник, чья это машина (Boarding/Working) — гейт водителя. -1 — не наша.
    int workerOfVehicle(int vehicleId) const;

    // --- кулдаун лечения (принадлежит пациенту) ---
    bool canHeal(int patientId, TimePoint now) const;
    // Сколько секунд кулдауна осталось (0 — можно лечить).
    int healCooldownLeft(int patientId, TimePoint now) const;

  private:
    // --- вызывается ТОЛЬКО MedicJobSystem ---

    enum class StartResult
    {
        AlreadyWorking,
        Boarding, // нашлась свободная точка спавна -> машина выдаётся сразу
        Queued    // свободных точек нет -> в очередь
    };
    struct StartOutcome
    {
        StartResult result;
        int spot = -1;
        int queuePosition = 0;
    };

    // usable(spot) — «на точке физически свободно» (привод спрашивает мир: чужая
    // машина могла встать на место, и спавн внутрь неё — то, что видит игрок).
    // Сервис о мире не знает, поэтому проверку приносит вызывающий.
    using SpotUsable = std::function<bool(int spot)>;

    StartOutcome startWork(int playerId, const SpotUsable &usable);

    // Привязать заспавненную машину к работнику (после успешного create).
    void setVehicle(int playerId, int vehicleId);

    // Машина отъехала с точки спавна: Boarding -> Working, точка свободна.
    // no-op вне Boarding.
    void completeBoarding(int playerId);

    // Провал посадки: снять точку и машину, работник в КОНЕЦ очереди.
    void requeueTail(int playerId);

    struct Promotion
    {
        int playerId = -1;
        int spot = -1;
    };
    // Продвинуть голову очереди на свободную точку (phase -> Boarding).
    Promotion promoteQueue(const SpotUsable &usable);

    // Зафиксировать лечение пациента: кулдаун стартует с этого момента.
    void markHealed(int patientId, TimePoint now);

    // Завершить смену/увольнение: снять точку, убрать из очереди, phase ->
    // NotWorking. Кошелёк и кулдаун лечения НЕ трогает. Идемпотентно.
    void endShift(int playerId);

    // Сброс кулдауна лечения слота (конец сессии).
    void resetPatient(int playerId);

    struct State
    {
        Phase phase = Phase::NotWorking;
        int vehicleId = -1; // выданная скорая (Boarding/Working)
        int spot = -1;      // удерживаемая точка спавна (только Boarding)
    };

    int firstFreeSpot(const SpotUsable &usable) const;
    void removeFromQueue(int playerId);
    void releaseSpot(int playerId);

    std::array<State, MAX_PLAYERS> m_state{};
    std::array<int, SPOT_COUNT> m_spotHolder{}; // playerId держателя или -1
    // Момент последнего лечения ПАЦИЕНТА (не врача). Дефолтный TimePoint{} — «не
    // лечили никогда», кулдаун не действует.
    std::array<TimePoint, MAX_PLAYERS> m_healedAt{};
    std::deque<int> m_queue;
};
