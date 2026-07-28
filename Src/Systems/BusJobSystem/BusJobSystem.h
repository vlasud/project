#pragma once

#include "Macro.h"
#include "Services/BusJobService/BusJobService.h"
#include "Services/BusWalletService/BusWalletService.h"
#include "Services/JobDismissService/JobDismissService.h"
#include "Services/JobWalletService/JobWalletService.h"
#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/Core/AudioService/AudioService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/NavigationLockService/NavigationLockService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/Core/ScreenTimerService/ScreenTimerService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>

// Работа-водитель автобуса (бизнес-фича, НЕ Core) — привод BusJobService по паттерну
// порта. Депо ФИЗИЧЕСКОЕ: 3 площадки, на каждой ВСЕГДА стоит незарезервированный
// pre-stock автобус. Геймплей событийный + один общий per-second таймер депо (не
// per-tick):
//  * пикап трудоустройства -> диалог «Начать работу»/«Информация»/«Забрать
//    деньги»/«Завершить работу» (все пункты видны всегда, гейт в обработчике);
//  * «Начать работу» -> есть свободный СТОЯЩИЙ автобус -> он закрепляется за игроком
//    (Reserved); нет свободного стоящего -> FIFO-очередь. Автобус НЕ спавнится под
//    заказ — он уже стоит в депо;
//  * с момента резерва 30 с сесть за руль СВОЕГО (отмеченного красным маркером)
//    автобуса И подобрать первый чекпоинт (иначе: стоит на площадке -> оставляем
//    pre-stock, уведён -> деспавн; работник — в КОНЕЦ очереди);
//  * маршрут — 54 race-чекпоинта по кругу (move = RACE_NORMAL со стрелкой на следующую
//    точку, stop = RACE_FINISH без стрелки); зачёт ТОЛЬКО за рулём СВОЕГО
//    автобуса. move: подобрал -> зачёт; stop: простоять 10 с в зоне за рулём -> зачёт.
//    Каждый зачёт +$50 в персистентный кошелёк (BusWalletService, write-through);
//    полный круг (54 подряд) +$2000 туда же (топливо у автобусов бесконечное);
//  * посадка завершена (первый чекпоинт за рулём) -> Driving: автобус покидает депо,
//    площадка освобождается и пере-стокуется (новый pre-stock, как только точка
//    физически чиста). Водителей на маршруте одновременно — сколько угодно;
//  * вышел из автобуса в смене -> 30 с вернуться за руль, иначе увольнение; смерть/
//    дисконнект -> увольнение сразу; «Завершить работу» -> штатное увольнение;
//    увольнение деспавнит едущий/уведённый автобус (не бросаем хлам), стоящий на
//    площадке резервный — оставляет pre-stock; кошелёк НИКОГДА не трогает;
//  * «Забрать деньги» переводит весь кошелёк на руки (PlayerMoneyService).
//
// Тик депо (per-second, O(SLOT_COUNT) на депо-часть): реконсиляция/пере-сток пустых
// физически чистых площадок (многопробный критерий занятости, как
// ParkingSystem::isSpotFree), продвижение очереди на освободившиеся стоящие автобусы,
// окна активных работников (посадка/возврат/остановка/анти-сквоттинг).
//
// Привязка автобуса к игроку — ТОЛЬКО РУЛЬ (правило владельца): за руль закреплённого
// (Reserved/Driving) автобуса пускается ТОЛЬКО его работник, за руль незарезервированного
// pre-stock — НИКТО (ждёт резерва). Пассажирские места СВОБОДНЫ ДЛЯ ВСЕХ у любого автобуса
// (стоящего/зарезервированного/едущего): замков дверей и высадки пассажиров нет вообще.
// Гейт один — серверный (subscribeDriverGate по occupant-трекингу VehicleService: не тот
// игрок стал водителем -> высадка removeFromVehicle; клиенту не верим). Перенос резерва
// убран: сесть за руль можно ТОЛЬКО в свой отмеченный маркером автобус. Клиенту не
// доверяем: «за рулём своего автобуса» — по серверному VehicleService::getDriver, вход в
// чекпоинт — по принятой сервером позиции (CheckpointService), занятость площадки —
// серверный расчёт (anyVehicleNear).
class BusJobSystem : public BaseSystem
{
  public:
    BusJobSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    // --- пикап + диалог ---
    void onPickup(IPlayer &player);
    void onStartWork(IPlayer &player);
    void onFinishWork(IPlayer &player);
    void onWithdrawMoney(IPlayer &player);
    void showInfo(IPlayer &player);

    // Загрузка кошелька автобусника по старту сессии (serial-guard).
    void loadWallet(IPlayer &player, const PlayerSessionService::Session &session);
    // Конец сессии/дисконнект: тихий teardown смены (без сообщений) + деспавн едущего
    // автобуса (стоящий резервный оставляем pre-stock) + сброс кэша кошелька (БД не
    // трогаем).
    void onSessionEnd(IPlayer &player);
    // Смерть в смене (серверно-авторитетно, PlayerHealthService::subscribeDeath):
    // немедленное увольнение с сообщением.
    void onPlayerDeath(IPlayer &player);

    // --- гейт посадки (только руль) ---
    // За руль закреплённого автобуса — только его работник; за руль pre-stock — никто.
    // Пассажирские места НЕ гейтим (свободны для всех). Бэкстоп — серверная высадка:
    // при отказе гейта VehicleService сам делает removeFromVehicle (force).
    bool onDriverGate(IPlayer &player, IVehicle &vehicle);

    // --- резерв/очередь ---
    // Реакция на закрепление автобуса за игроком (startWork/промоушен): показать
    // первый чекпоинт + запустить окно посадки. Автобус уже стоит — не спавним.
    void onReserved(IPlayer &player);
    // Сел за руль поданного автобуса: Reserved -> Driving, открывается маршрут.
    // Окно выезда при этом ПРОДОЛЖАЕТ идти — оно про освобождение площадки.
    void completeBoarding(IPlayer &player);
    // Продвинуть очередь на все освободившиеся стоящие автобусы (пере-сток/снятый
    // резерв). Каждому продвинутому — onReserved; ждущим — сдвиг мест.
    void pumpQueue();
    // Уведомить ждущих об обновлённом месте после сдвига очереди (голову — что она
    // следующая). O(длины очереди), только по событию сдвига.
    void notifyQueueShift();

    // --- маршрут ---
    void onCheckpointEnter(IPlayer &player);
    void creditAndAdvance(IPlayer &player);
    void showRouteCheckpoint(IPlayer &player);

    // --- таймер депо (один общий, per-second) ---
    void onDepotTick();
    // Пере-сток: на каждой пустой физически чистой площадке заспавнить pre-stock
    // автобус; реконсиляция уничтоженного извне стоящего автобуса. O(SLOT_COUNT).
    // Освободить площадки, с которых автобус уже уехал (или пропал): pre-stock в депо
    // нет, площадку держит машина конкретного работника.
    void releaseDepartedSpots();
    // Подать работнику автобус на его площадку. false — не смогли (пул полон).
    bool spawnForWorker(IPlayer &player);
    // Прогнать окна активных работников (Reserved/Driving) по онлайну.
    void tickActiveWorkers();
    void tickWorker(IPlayer &player);

    // --- увольнение/провал посадки ---
    // Провал посадки (30 с не сел/не доехал): работник в КОНЕЦ очереди; автобус стоит
    // на площадке -> оставить pre-stock, уведён с площадки -> деспавн.
    void failBoarding(IPlayer &player);
    // Штатное увольнение: teardown смены + сообщение (info для добровольного «Завершить
    // работу», error для смерти/тайм-аута). Кошелёк НЕ трогает.
    void dismiss(IPlayer &player, const std::string &reason, const Colour &colour);
    // Общий teardown смены БЕЗ сообщения: снять чекпоинт, отвязать автобус (едущий/
    // уведённый — деспавн; резервный на площадке — оставить pre-stock), снять
    // резерв/очередь, обнулить счётчики.
    void teardownShift(IPlayer &player);

    // --- helpers ---
    bool drivingOwnBus(int playerId) const; // за рулём СВОЕГО закреплённого/угнанного автобуса
    // Автобус vid физически стоит в радиусе своей площадки (2D, Z игнорируем).
    bool busOnSpot(int vid, int spot) const;
    // Площадка spot физически чиста для пере-стока: многопробная занятость ЛЮБЫМ ТС
    // (центр + вперёд/назад по SLOT_ANGLE), как ParkingSystem::isSpotFree.
    bool spotClear(int spot) const;
    void clearCounters(int playerId); // обнулить секундные счётчики окна

    BusJobService &m_busJobService;
    BusWalletService &m_busWalletService;
    JobWalletService &m_jobWalletService;
    VehicleService &m_vehicleService;
    CheckpointService &m_checkpointService;
    PickupService &m_pickupService;
    PlayerMoneyService &m_moneyService;
    PlayerStateService &m_stateService;
    PlayerLocationService &m_locationService;
    PlayerDialogService &m_dialogService;
    PlayerSessionService &m_sessionService;
    PlayerHealthService &m_healthService;
    TimerService &m_timers;
    ScreenNoticeService &m_screenNoticeService;
    ScreenTimerService &m_screenTimerService;
    MapIconService &m_mapIconService;
    AudioService &m_audioService;
    VehicleWaypointService &m_waypointService; // красный чекпоинт-маркер на закреплённый автобус
    NavigationLockService &m_navLockService;   // лок навигации на всю смену (GPS недоступен)

    int m_pickup = -1;                 // хэндл пикапа трудоустройства
    TimerService::Handle m_depotTimer; // общий per-second таймер депо (на весь сервер)

    // Секундные счётчики окон (индексируются playerId; ведёт per-second тик):
    std::array<int, MAX_PLAYERS> m_reserveSeconds{}; // с момента резерва (окно посадки)
    std::array<int, MAX_PLAYERS> m_exitSeconds{};    // вне руля своего автобуса в смене (окно возврата)
    std::array<int, MAX_PLAYERS> m_dwellSeconds{};   // простой в зоне stop-чекпоинта за рулём
    std::array<int, MAX_PLAYERS> m_idleSeconds{};    // за рулём без зачёта чекпоинта (окно анти-сквоттинга)
};
