#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include <array>
#include <deque>
#include <functional>
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
    // ПОТОЛОК грузовиков работы в мире одновременно: pre-stock на площадках плюс
    // выданные работникам. Упор в потолок гасит пере-сток депо, и очередь работает
    // сама собой — свободных стоящих грузовиков просто не появляется. Рабочих мест
    // при этом вдвое больше потолка: на грузовик приходятся водитель и грузчик.
    static constexpr int MAX_TRUCKS = 16;
    // У плеча езды РОВНО ОДИН чекпоинт — конец плеча (порт / база / точка заказа).
    // Промежуточной разметки нет: дорогу водитель выбирает сам, поэтому индекса
    // чекпоинта в состоянии тоже нет.
    static constexpr int BOXES_PER_LEG = 10; // коробок за плечо погрузки/разгрузки

    // Роль в работе. Смену (грузовик, маршрут, фазу, счётчик коробок) ведёт ВОДИТЕЛЬ;
    // грузчик своей фазы не имеет — он обслуживает смену напарника. Соло-водитель
    // совмещает обе роли: и едет, и носит.
    enum class Role
    {
        None,
        Driver,
        Loader
    };

    enum class Phase
    {
        NotWorking, // не в смене
        Queued,     // работник, ждёт свободного стоящего грузовика в FIFO-очереди
        Reserved,   // грузовик закреплён на площадке, ждём посадку за руль
        Idle,       // за рулём, ЦЕЛЬ РЕЙСА НЕ ВЫБРАНА (/target) — держит грузовик
        DriveOut,   // едет в порт (один чекпоинт — сам порт)
        Loading,    // пешком грузит коробки в грузовик
        DriveBack,  // едет к месту разгрузки (один чекпоинт — база / точка заказа)
        Unloading,  // пешком разгружает коробки
        Standby     // грузчик: устроен и ждёт пары либо носит коробки в смене напарника
    };

    // ЧТО за рейс делает смена. Фазы у обоих режимов ОДНИ И ТЕ ЖЕ — меняются только
    // концы плеч, поэтому режим это поле смены, а не новая ветка фазовой машины:
    //   Port:   Idle -> DriveOut(порт) -> Loading(склад->кузов) -> DriveBack(база)
    //           -> Unloading(->точка выгрузки базы) -> Idle
    //   Orders: Idle ->      —        -> Loading(база->кузов)   -> DriveBack(точка
    //           бизнеса) -> Unloading(->прилавок внутри) -> Idle
    // Плечо в порт заказу не нужно: товар лежит на базе. КАЖДЫЙ рейс кончается фазой
    // Idle: цель выбирается заново (/target), грузовик при этом остаётся за водителем.
    enum class Mode
    {
        Port,  // рейс в порт и обратно
        Orders // заказ бизнеса: база -> точка владельца
    };

    Phase phaseOf(int playerId) const;
    // Режим и заказ — свойства СМЕНЫ, то есть читаются по shiftOwnerOf (у грузчика
    // своих нет: он обслуживает рейс напарника).
    Mode modeOf(int playerId) const;
    int orderIdOf(int playerId) const; // id заказа BusinessOrderService; 0 — нет
    Role roleOf(int playerId) const;
    int partnerOf(int playerId) const; // напарник по паре; -1 — соло/без пары

    // Водитель смены, которую обслуживает игрок: он сам (роль водителя с грузовиком)
    // либо его напарник (роль грузчика). -1 — игрок не участвует в активной смене.
    // Фаза, грузовик и boxCount читаются ПО ЭТОМУ id, а не по игроку.
    int shiftOwnerOf(int playerId) const;
    // Кто носит коробки в смене водителя: напарник-грузчик, иначе сам водитель.
    // Пара делит труд — водитель в паре коробки не трогает.
    int carrierOf(int driverId) const;
    bool isWorking(int playerId) const;        // phase != NotWorking
    int vehicleIdOf(int playerId) const;       // -1 — нет грузовика (очередь)
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
    // Кто держит площадку (грузовик выдан ему и ещё стоит на ней), или -1.
    int holderOfSpot(int spot) const;
    // Сколько грузовиков работы существует прямо сейчас: стоящие на площадках (в т.ч.
    // закреплённые за игроками на посадке) плюс уехавшие с депо. Сверяется с
    // MAX_TRUCKS перед пере-стоком.
    int truckCount() const;

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

    // usable(spot) — «на площадке физически свободно» (привод спрашивает мир: место
    // могла занять чужая машина, а спавн внутрь неё — то, что видит игрок). Сервис о
    // мире не знает, проверку приносит вызывающий.
    using SpotUsable = std::function<bool(int spot)>;

    // Устроить ВОДИТЕЛЕМ: грузовик закрепляется сразу (или очередь), ЦЕЛЬ рейса не
    // выбрана — её водитель берёт уже за рулём (Idle -> startPortRun/startOrderLeg).
    StartOutcome startWork(int playerId, const SpotUsable &usable);
    // Заказ отработан либо возвращён в пул — снять привязку, чтобы teardown не вернул
    // в пул то, чего уже нет. Режим сбрасывается на Port: без заказа режим Orders не
    // означал бы ничего, а цель всё равно выбирается заново.
    void clearOrder(int playerId);
    // Цель выбрана — ПОРТ: Idle -> DriveOut, режим Port, заказ снят, счётчики с нуля.
    // false — фаза не Idle (цель выбирают только без активной задачи) либо нет
    // грузовика. Гейты «за рулём» и «на базе» держит привод: они про мир, не про стейт.
    bool startPortRun(int playerId);
    // Цель выбрана — ЗАКАЗ: Idle -> Loading (груз заказа лежит на базе), режим Orders.
    // Заказ к этому моменту УЖЕ принят в BusinessOrderService (иначе его успел бы
    // взять второй водитель), поэтому здесь он только запоминается: сервис работы про
    // состав и премию не знает. false — фаза не Idle, нет грузовика либо заказ уже есть.
    bool startOrderLeg(int playerId, int orderId);

    // Устроить ГРУЗЧИКОМ: NotWorking -> Standby, роль Loader. Ни грузовика, ни
    // площадки, ни очереди — грузчик ждёт приглашения водителя. false — уже работает.
    bool startLoader(int playerId);

    // Связать водителя и грузчика. Отказ (false), если роли/фазы не те, кто-то уже
    // в паре или это один и тот же игрок. Проверки здесь, чтобы привод не мог
    // собрать пару в обход правил.
    bool makePair(int driverId, int loaderId);
    // Разорвать пару обеих сторон. Возвращает бывшего напарника (-1 — пары не было).
    // Идемпотентно; вызывается из endShift/requeueTail, поэтому пара не переживает
    // конец смены ни одной из сторон.
    int breakPair(int playerId);
    // Снять флаг «несёт коробку» (teardown напарника при распаде пары).
    void clearCarry(int playerId);

    // Сел за руль: Reserved -> Idle (цель рейса ещё не выбрана). no-op вне Reserved.
    void completeBoarding(int playerId);

    // Доехал до порта (вошёл в чекпоинт плеча «туда»): DriveOut -> Loading,
    // boxCount=0, carrying=false. no-op вне DriveOut.
    void beginLoading(int playerId);
    // Погрузка окончена (10/10): Loading -> DriveBack. no-op вне Loading.
    void beginDriveBack(int playerId);
    // Доехал до места разгрузки (база / точка заказа): DriveBack -> Unloading,
    // boxCount=0, carrying=false. no-op вне DriveBack.
    void beginUnloading(int playerId);
    // Разгрузка окончена (10/10): Unloading -> Idle, boxCount=0. Рейс сдан, грузовик
    // остаётся за водителем, цель выбирается заново. no-op вне Unloading.
    void finishRun(int playerId);

    // Взял коробку (вход в чекпоинт-источник пешей фазы): carrying false->true у
    // НОСИЛЬЩИКА. no-op, если он не носильщик смены, вне Loading/Unloading или уже несёт.
    void beginCarry(int carrierId);
    // Сдал коробку (вход в чекпоинт-приёмник): carrying true->false у носильщика,
    // ++boxCount в смене ВОДИТЕЛЯ (прогресс принадлежит рейсу, а не человеку).
    // Возвращает НОВЫЙ boxCount; 0 (no-op) — не нёс / вне Loading/Unloading.
    int finishCarry(int carrierId);

    // Записать на площадку выданный работнику грузовик (после успешного спавна).
    void setStanding(int spot, int vehicleId);
    // Привязать выданный грузовик к работнику (после успешного спавна).
    void setVehicle(int playerId, int vehicleId);
    // Освободить площадку: грузовик уехал с неё либо пропал. Идемпотентно.
    void releaseSpot(int playerId);

    struct Promotion
    {
        int playerId = -1;
        int vehicleId = -1;
    };
    // Продвинуть голову очереди на свободную площадку (phase -> Reserved). Грузовик
    // спавнит привод и регистрирует его через setStanding.
    Promotion promoteQueue(const SpotUsable &usable);

    // Провал посадки: снять площадку, работник в КОНЕЦ очереди (phase -> Queued).
    // Грузовик деспавнит привод — pre-stock в этой модели нет.
    void requeueTail(int playerId);

    // Завершить смену/увольнение: снять площадку, убрать из очереди, phase ->
    // NotWorking, обнулить состояние. Кошелёк НЕ трогает. Идемпотентно.
    void endShift(int playerId);

    struct State
    {
        Phase phase = Phase::NotWorking;
        Role role = Role::None;
        Mode mode = Mode::Port; // режим ТЕКУЩЕГО рейса (в Idle не значит ничего)
        int partnerId = -1;   // вторая половина пары; -1 — соло/без пары
        int vehicleId = -1;   // закреплённый (Reserved) / ведомый (Idle..Unloading) грузовик
        int boxCount = 0;     // коробок сдано на текущем плече (ведёт ВОДИТЕЛЬ смены)
        int orderId = 0;      // заказ бизнеса в режиме Orders; 0 — нет
        bool carrying = false; // держит коробку — флаг НОСИЛЬЩИКА, не смены
    };
    struct Spot
    {
        int vehicleId = -1;
        int reservedBy = -1;
    };

    // Держит ли фаза личный грузовик игрока (для workerOfVehicle/детача).
    static bool holdsVehicle(Phase phase);

    int firstFreeSpot(const SpotUsable &usable) const;
    void detachVehicle(int playerId);
    void removeFromQueue(int playerId);

    std::array<State, MAX_PLAYERS> m_state{};
    std::array<Spot, SLOT_COUNT> m_spots{};
    std::deque<int> m_queue;
};
