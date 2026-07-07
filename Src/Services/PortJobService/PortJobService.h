#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include <array>

class PortJobSystem;

// Работа-грузчик в порту — бизнес-фича (НЕ Core). Источник правды о пер-player
// машине состояний цикла «идёт к кораблю за ящиком -> несёт его на склад» и о
// балансировщике занятости 6 точек сброса (сколько работников СЕЙЧАС несут на
// каждую точку — чтобы не скапливать всех на одной). Побочные эффекты (пикап,
// персональный чекпоинт, анимация, attach ящика, выплата) выполняет привод
// PortJobSystem; сервис только считает состояние.
class PortJobService final : public IService
{
    friend PortJobSystem;

  public:
    static constexpr int DROP_COUNT = 6;
    static constexpr unsigned long long PAY_PER_BOX = 100;

    enum class Phase
    {
        NotWorking, // не в смене
        GoToSource, // идёт к чекпоинту корабля за ящиком
        Carrying    // несёт ящик к назначенной точке сброса
    };

    Phase phaseOf(int playerId) const;
    bool isWorking(int playerId) const;
    int assignedSpotOf(int playerId) const; // -1 — нет назначенной точки (не Carrying)
    int deliveredOf(int playerId) const;    // ящиков отнесено за смену, ещё не выплачено

  private:
    // --- вызывается ТОЛЬКО PortJobSystem (мутирующие переходы фазы) ---

    // Начать смену: NotWorking -> GoToSource. false — уже работает / bounds-промах.
    bool startWork(int playerId);

    // Взял ящик у источника: балансировщик назначает точку сброса — приоритет (1)
    // минимальная текущая занятость среди DROP_COUNT, (2) среди мин.-загруженных —
    // не та же точка, что в прошлый раз у ЭТОГО игрока (разнообразие маршрута), (3)
    // тай-брейк — меньший индекс. ++занятость точки, запоминаем её как lastSpot,
    // GoToSource -> Carrying. Возвращает индекс назначенной точки (0..DROP_COUNT-1)
    // или -1 (не в фазе GoToSource / bounds-промах).
    int assignDropSpot(int playerId);

    // Донёс ящик до назначенной точки: --занятость точки, ++отнесено за смену,
    // Carrying -> GoToSource. false (no-op) — не в фазе Carrying / bounds-промах.
    bool completeDelivery(int playerId);

    // Уронить несомый ящик (смерть в фазе Carrying): освободить слот распределения,
    // Carrying -> GoToSource; delivered СОХРАНЯЕТСЯ (смена не прерывается). no-op вне
    // Carrying. Привод сам заново поставит чекпоинт источника на респавне.
    void dropCarry(int playerId);

    // Завершить смену («Завершить работу»): возвращает сумму к выплате
    // (delivered*PAY_PER_BOX), счётчик отнесённых обнуляется, слот распределения
    // освобождается (если был занят — застали в Carrying), фаза -> NotWorking.
    // 0, если игрок не работал.
    unsigned long long endWork(int playerId);

    // Сброс на конце сессии/дисконнекте: освобождает слот распределения (если был
    // занят) и обнуляет состояние БЕЗ выплаты — деньги за незавершённую смену
    // сгорают (выплата только через endWork, «на руки при увольнении»).
    // Идемпотентно; bounds-safe.
    void resetPlayer(int playerId);

    struct State
    {
        Phase phase = Phase::NotWorking;
        int assignedSpot = -1;
        int delivered = 0;
        int lastSpot = -1; // точка предыдущей сдачи (для разнообразия в assignDropSpot); -1 — ещё не сдавал
    };

    // Снять занятость точки состояния (если была назначена) ровно один раз — гейт
    // по assignedSpot != -1, иначе m_assigned ушёл бы в минус при повторном вызове
    // (например endWork после уже случившегося completeDelivery).
    void releaseSpot(State &state);

    std::array<State, MAX_PLAYERS> m_state;
    std::array<int, DROP_COUNT> m_assigned{}; // сколько игроков СЕЙЧАС несут на каждую точку
};
