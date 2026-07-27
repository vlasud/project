#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include <array>
#include <deque>
#include <vector>

class HaulerJobSystem;

// Работа-развозчик (портовый хаулер) — бизнес-фича (НЕ Core). Источник правды о:
//  * пер-player фазе цикла смены (не работает / в очереди / грузовик закреплён на
//    посадку / едет в порт / грузит на складе / едет обратно / разгружает на базе)
//    и о прогрессе внутри фазы (индекс чекпоинта маршрута ИЛИ счётчик коробок);
//  * состоянии SLOT_COUNT ФИЗИЧЕСКИХ площадок депо: на каждой стоит pre-stock
//    грузовик, либо он закреплён за игроком (reserved), либо площадка пуста;
//  * FIFO-очереди работников, ждущих освобождения стоящего грузовика.
//
// Депо-модель — КЛОН BusJobService (площадки/пре-сток/резерв/очередь): грузовик
// стоит на площадке, пока не уехал (посадка завершена -> DriveOut, покинул депо).
// SLOT_COUNT — пропускная способность депо, а НЕ кап одновременных работников на
// маршруте. РАСХОЖДЕНИЕ с автобусом — маршрут не кольцевой race, а МНОГОФАЗНЫЙ цикл
// (езда-туда -> пешая погрузка 10 коробок -> езда-обратно -> пешая разгрузка 10
// коробок -> заново). Общий депо-каркас с BusJobService НЕ обобщён сознательно
// (кандидат в Docs/Refactoring.md) — обобщение задело бы рабочую фичу автобуса.
//
// Побочные эффекты (спавн/деспавн грузовиков, чекпоинты, анимация/attach коробки,
// начисление денег, таймеры, попапы) выполняет привод HaulerJobSystem; сервис
// только считает состояние и решает распределение резервов/очереди/фаз.
class HaulerJobService final : public IService
{
    friend HaulerJobSystem;

  public:
    // Размер депо: число физических площадок и стоящих на них pre-stock грузовиков
    // (рычаг «стартовой пропускной способности», НЕ потолок одновременных
    // работников — уехавший освобождает площадку под следующего).
    static constexpr int SLOT_COUNT = 4;
    // Чекпоинтов на каждом плече езды (задаются приводом; сервис лишь ведёт индекс
    // и знает длину для клампа перехода в пешую фазу на последней точке плеча).
    static constexpr int OUT_LENGTH = 13;  // езда-туда: 13 замеров, последний запускает погрузку
    static constexpr int BACK_LENGTH = 7;  // езда-обратно: 7 замеров, последний запускает разгрузку
    static constexpr int BOXES_PER_LEG = 10; // коробок за плечо погрузки/разгрузки

    enum class Phase
    {
        NotWorking, // не в смене
        Queued,     // работник, ждёт свободного стоящего грузовика в FIFO-очереди
        Reserved,   // грузовик закреплён на площадке, ждём посадку + первый чекпоинт
        DriveOut,   // едет в порт по чекпоинтам туда (площадку не держит)
        Loading,    // пешком грузит коробки со склада порта в грузовик
        DriveBack,  // едет обратно на базу по чекпоинтам
        Unloading   // пешком разгружает коробки на точку выгрузки базы
    };

    Phase phaseOf(int playerId) const;
    bool isWorking(int playerId) const;        // phase != NotWorking
    int vehicleIdOf(int playerId) const;       // -1 — нет грузовика (очередь)
    int driveIndexOf(int playerId) const;      // индекс чекпоинта текущего плеча езды
    int boxCountOf(int playerId) const;        // коробок сдано на текущем плече (0..BOXES_PER_LEG)
    bool carryingOf(int playerId) const;       // держит коробку (ждём сброса) в Loading/Unloading
    int queuePositionOf(int playerId) const;   // 1-based место в очереди; 0 — не в очереди
    std::vector<int> queuedPlayers() const;    // снимок FIFO-очереди для уведомлений о сдвиге

    // --- депо (площадки) ---
    int standingVehicle(int spot) const;       // id стоящего грузовика площадки или -1
    bool spotReserved(int spot) const;         // закреплён ли стоящий грузовик за работником
    int reservedSpotOf(int playerId) const;    // площадка, чей грузовик закреплён за игроком, или -1
    // Работник, за кем закреплён (Reserved) ИЛИ кто ведёт (DriveOut/Loading/
    // DriveBack/Unloading) грузовик vehicleId — гейт водителя. -1, если это не
    // резервный/личный грузовик игрока. Опора на РЕАЛЬНЫЙ id грузовика, не на
    // owner-тег (Owner::Work занимают и автобусы, и дев-машины).
    int workerOfVehicle(int vehicleId) const;
    // Свободный (незарезервированный) стоящий грузовик депо?
    bool isFreeStandingVehicle(int vehicleId) const;

  private:
    // --- вызывается ТОЛЬКО HaulerJobSystem (мутирующие переходы) ---

    enum class StartResult
    {
        AlreadyWorking,
        Reserved, // есть свободный стоящий грузовик -> закреплён за игроком
        Queued    // свободных стоящих нет -> в очередь
    };
    struct StartOutcome
    {
        StartResult result;
        int vehicleId = -1;
        int queuePosition = 0;
    };

    StartOutcome startWork(int playerId);

    // Посадка завершена (первый чекпоинт туда подобран за рулём): Reserved ->
    // DriveOut, площадка освобождается (грузовик покинул депо). no-op вне Reserved.
    void completeBoarding(int playerId);

    // Зачёт чекпоинта езды: ++driveIndex. Возвращает НОВЫЙ индекс. no-op вне
    // DriveOut/DriveBack (возвращает текущий индекс).
    int advanceDrive(int playerId);

    // Плечо езды-туда доехало (вошли в последнюю точку OUT): DriveOut -> Loading,
    // boxCount=0, carrying=false. no-op вне DriveOut.
    void beginLoading(int playerId);
    // Погрузка окончена (10/10): Loading -> DriveBack, driveIndex=0. no-op вне Loading.
    void beginDriveBack(int playerId);
    // Плечо езды-обратно доехало (вошли в последнюю точку BACK): DriveBack ->
    // Unloading, boxCount=0, carrying=false. no-op вне DriveBack.
    void beginUnloading(int playerId);
    // Разгрузка окончена (10/10): Unloading -> DriveOut, driveIndex=0, boxCount=0.
    // Цикл начинается заново. no-op вне Unloading.
    void completeCycle(int playerId);

    // Взял коробку (вход в чекпоинт-источник пешей фазы): carrying false->true.
    // no-op вне Loading/Unloading или если уже несёт.
    void beginCarry(int playerId);
    // Сдал коробку (вход в чекпоинт-приёмник): carrying true->false, ++boxCount.
    // Возвращает НОВЫЙ boxCount; 0 (no-op) — не нёс / вне Loading/Unloading.
    int finishCarry(int playerId);

    // Пере-сток: привязать свежий pre-stock грузовик к пустой площадке.
    void setStanding(int spot, int vehicleId);
    // Освободить площадку (реконсиляция уничтоженного извне pre-stock грузовика).
    void clearStanding(int spot);

    struct Promotion
    {
        int playerId = -1;
        int vehicleId = -1;
    };
    // Продвинуть голову очереди на свободный стоящий грузовик (phase -> Reserved).
    Promotion promoteQueue();

    // Провал посадки: снять резерв, работник в КОНЕЦ очереди (phase -> Queued).
    // busKept=true — грузовик остаётся pre-stock; false — площадка освобождается.
    void requeueTail(int playerId, bool busKept);

    // Завершить смену/увольнение: снять резерв площадки, убрать из очереди, phase
    // -> NotWorking, обнулить состояние. Кошелёк НЕ трогает. Идемпотентно.
    void endShift(int playerId, bool busKept);

    struct State
    {
        Phase phase = Phase::NotWorking;
        int vehicleId = -1;   // закреплённый (Reserved) / ведомый (DriveOut..Unloading) грузовик
        int driveIndex = 0;   // индекс чекпоинта текущего плеча езды
        int boxCount = 0;     // коробок сдано на текущем плече погрузки/разгрузки
        bool carrying = false; // держит коробку (ждём сброса) в Loading/Unloading
    };
    struct Spot
    {
        int vehicleId = -1;
        int reservedBy = -1;
    };

    // Держит ли фаза личный грузовик игрока (для workerOfVehicle/детача).
    static bool holdsVehicle(Phase phase);

    int firstFreeStandingSpot() const;
    void detachVehicle(int playerId, bool busKept);
    void removeFromQueue(int playerId);

    std::array<State, MAX_PLAYERS> m_state{};
    std::array<Spot, SLOT_COUNT> m_spots{};
    std::deque<int> m_queue;
};
