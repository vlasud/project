#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include <array>
#include <deque>
#include <functional>
#include <vector>

class BusJobSystem;

// Работа-водитель автобуса — бизнес-фича (НЕ Core). Источник правды о:
//  * пер-player фазе цикла смены (не работает / в очереди / автобус закреплён и ждёт
//    посадки / едет по маршруту) и его прогрессе по маршруту (индекс текущего
//    чекпоинта 0..ROUTE_LENGTH-1);
//  * состоянии SLOT_COUNT ФИЗИЧЕСКИХ площадок депо: на каждой стоит незарезервированный
//    (pre-stock) автобус, либо автобус закреплён за игроком на посадку (reserved),
//    либо площадка пуста (ждёт пере-стока приводом);
//  * FIFO-очереди работников, ждущих освобождения стоящего автобуса.
//
// НОВАЯ МОДЕЛЬ занятости — ФИЗИЧЕСКАЯ: площадка занята, пока на её точке стоит рабочий
// автобус; уехал (посадка завершена -> Driving, автобус покинул депо) — площадка
// освобождается и пере-стокуется. SLOT_COUNT — размер депо (пропускная способность,
// число pre-stock автобусов), а НЕ кап одновременных водителей: Driving-водитель
// площадку НЕ держит, водителей на маршруте может быть сколько угодно.
//
// Побочные эффекты (спавн/деспавн автобусов, физическая проверка занятости точки,
// персональный чекпоинт, попапы, начисление денег, таймеры) выполняет привод
// BusJobSystem; сервис только считает состояние и решает распределение резервов/
// очереди. Автобусы существуют ТОЛЬКО под работу (нет постоянных): pre-stock стоят в
// депо, reserved закреплён на посадку, driving угнан водителем и деспавнится в конце
// смены.
class BusJobService final : public IService
{
    friend BusJobSystem;

  public:
    // Размер депо: число физических площадок и стоящих на них pre-stock автобусов.
    // Это рычаг баланса «стартовой пропускной способности» (сколько водителей могут
    // выехать одновременно без ожидания), НО НЕ потолок одновременных работников на
    // маршруте — уехавший освобождает площадку под следующего. Рост числа площадок
    // ускоряет вход в премиальную по доходу работу — согласовывать с доходностью
    // автобуса (см. Docs/GameDesign/Economy.md, риск «Обесценивание порта»).
    static constexpr int SLOT_COUNT = 3;
    static constexpr int ROUTE_LENGTH = 54; // чекпоинтов на круге маршрута

    enum class Phase
    {
        NotWorking, // не в смене
        Queued,     // работник, ждёт свободного стоящего автобуса в FIFO-очереди
        Reserved,   // автобус закреплён на площадке, ждём посадку + первый чекпоинт
        Driving     // посадка завершена, едет по маршруту (площадку не держит)
    };

    Phase phaseOf(int playerId) const;
    bool isWorking(int playerId) const;        // phase != NotWorking
    int vehicleIdOf(int playerId) const;       // -1 — нет автобуса (резерв/езда)
    int checkpointIndexOf(int playerId) const; // 0..ROUTE_LENGTH-1
    int queuePositionOf(int playerId) const;   // 1-based место в очереди; 0 — не в очереди
    // Снимок FIFO-очереди (playerId в порядке ожидания) для уведомлений о её сдвиге.
    std::vector<int> queuedPlayers() const;

    // --- депо (площадки) ---
    // id стоящего автобуса депо на площадке (pre-stock или reserved) или -1 (пусто).
    int standingVehicle(int spot) const;
    // Закреплён ли стоящий автобус площадки за работником (reserved) — иначе он
    // свободный pre-stock. slot вне [0, SLOT_COUNT) -> false.
    bool spotReserved(int spot) const;
    // Площадка, чей автобус закреплён за playerId (Reserved), или -1 (для физической
    // проверки «автобус ещё стоит на своей площадке»). O(SLOT_COUNT).
    int reservedSpotOf(int playerId) const;
    // Работник, за кем закреплён (reserved) ИЛИ кто угнал (driving) автобус vehicleId
    // — гейт водителя. -1, если это не резервный/едущий автобус игрока (в т.ч. pre-stock
    // или чужой). Опора на РЕАЛЬНЫЙ id автобуса, а не на owner-тег VehicleService
    // (Owner::Work занимают и дев-машины).
    int workerOfVehicle(int vehicleId) const;
    // Свободный (незарезервированный) стоящий автобус депо? Гейт: в pre-stock автобус
    // за руль пускаем только работника, оформившего смену (иначе — угон стоящего).
    // Кто держит площадку (автобус подан ему и ещё стоит на ней), или -1.
    int holderOfSpot(int spot) const;

  private:
    // --- вызывается ТОЛЬКО BusJobSystem (мутирующие переходы) ---

    enum class StartResult
    {
        AlreadyWorking, // уже в смене/очереди — no-op
        Reserved,       // есть свободный стоящий автобус -> закреплён за игроком
        Queued          // свободных стоящих нет -> поставлен в очередь
    };
    struct StartOutcome
    {
        StartResult result;
        int vehicleId = -1;    // id закреплённого автобуса, валиден при Reserved
        int queuePosition = 0; // 1-based, валиден при Queued
    };

    // Начать работу: есть свободный стоящий автобус -> Reserved (закрепить его за
    // игроком); иначе Queued (хвост FIFO). Автобус НЕ спавнится — он уже стоит.
    // usable(spot) — «на площадке физически свободно» (привод спрашивает мир: место
    // могла занять чужая машина, а спавн внутрь неё — то, что видит игрок). Сервис о
    // мире не знает, проверку приносит вызывающий.
    using SpotUsable = std::function<bool(int spot)>;

    StartOutcome startWork(int playerId, const SpotUsable &usable);

    // Посадка завершена (первый чекпоинт подобран за рулём): Reserved -> Driving,
    // площадка освобождается (автобус покинул депо, стал личным едущим). no-op вне
    // Reserved.
    void completeBoarding(int playerId);

    // Зачёт текущего чекпоинта: ++индекс по модулю ROUTE_LENGTH. Возвращает НОВЫЙ
    // индекс; 0 — круг замкнулся (привод начисляет бонус за круг + заправку).
    int advanceCheckpoint(int playerId);

    // Пере-сток: привязать свежезаспавненный pre-stock автобус к пустой площадке.
    void setStanding(int spot, int vehicleId);
    // Привязать поданный автобус к работнику (после успешного спавна).
    void setVehicle(int playerId, int vehicleId);
    // Освободить площадку: автобус уехал с неё либо пропал. Идемпотентно.
    void releaseSpot(int playerId);
    // Освободить площадку (реконсиляция уничтоженного извне pre-stock автобуса).


    struct Promotion
    {
        int playerId = -1; // кого продвинули из очереди (-1 — нечего/некуда)
        int vehicleId = -1;
    };
    // Продвинуть голову очереди на свободный стоящий автобус (phase -> Reserved,
    // закрепить). {-1,-1} — очередь пуста или свободных стоящих автобусов нет.
    Promotion promoteQueue(const SpotUsable &usable);

    // Провал посадки: снять резерв, работник в КОНЕЦ очереди (остаётся работником,
    // phase -> Queued). busKept=true — автобус остаётся стоять pre-stock на площадке
    // (привод его НЕ деспавнит); false — площадка освобождается (привод деспавнит
    // автобус). Дедуп на случай двойного вызова.
    void requeueTail(int playerId);

    // Завершить работу/увольнение: снять резерв площадки (busKept как в requeueTail
    // для Reserved; для Driving площадки нет), убрать из очереди, phase -> NotWorking,
    // обнулить состояние. Кошелёк НЕ трогает. Идемпотентно; bounds-safe.
    void endShift(int playerId);

    struct State
    {
        Phase phase = Phase::NotWorking;
        int vehicleId = -1; // закреплённый (Reserved) / угнанный (Driving) автобус
        int cpIndex = 0;    // индекс текущего чекпоинта маршрута
    };
    // Физическая площадка депо.
    struct Spot
    {
        int vehicleId = -1;  // стоящий автобус (pre-stock/reserved) или -1 (пусто)
        int reservedBy = -1; // playerId резерва или -1 (свободный pre-stock)
    };

    int firstFreeSpot(const SpotUsable &usable) const; // ничья И физически пустая площадка, иначе -1
    void detachBus(int playerId); // отвязать автобус игрока от резерва/депо
    void removeFromQueue(int playerId);         // выкинуть из FIFO (дедуп/увольнение)

    std::array<State, MAX_PLAYERS> m_state{};
    std::array<Spot, SLOT_COUNT> m_spots{}; // площадки депо (в порядке SLOT_POS привода)
    std::deque<int> m_queue;                // FIFO работников, ждущих стоящий автобус
};
