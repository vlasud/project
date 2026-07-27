#pragma once

#include "Macro.h"
#include "Services/HaulerJobService/HaulerJobService.h"
#include "Services/HaulerWalletService/HaulerWalletService.h"
#include "Services/Core/AttachmentService/AttachmentService.h"
#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/NavigationLockService/NavigationLockService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/Core/ScreenTimerService/ScreenTimerService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/PortJobService/PortJobService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <random>

// Работа-развозчик (портовый хаулер, бизнес-фича — НЕ Core) — привод
// HaulerJobService. Депо ФИЗИЧЕСКОЕ (клон автобусного: SLOT_COUNT площадок, на
// каждой pre-stock грузовик Yankee, резерв + красный маркер + окно посадки 30 с,
// FIFO-очередь, взнос, серверный гейт руля «только своему», бесконечное топливо,
// лок навигации, взаимное исключение работ). РАСХОЖДЕНИЕ с автобусом — маршрут
// МНОГОФАЗНЫЙ, не кольцевой:
//  * ЕЗДА-ТУДА: 13 race-чекпоинтов, зачёт последнего запускает ПОГРУЗКУ;
//  * ПОГРУЗКА (пешком): 10 коробок со склада порта (общие точки PortJobService)
//    -> к ЗАДУ грузовика (чекпоинт от живой позиции/угла, пересчёт на коробку);
//  * ЕЗДА-ОБРАТНО: 7 race-чекпоинтов, зачёт последнего запускает РАЗГРУЗКУ;
//  * РАЗГРУЗКА (пешком): 10 коробок от зада грузовика -> точка выгрузки базы,
//    +$200 в кошелёк за КАЖДУЮ (write-through); после 10/10 бонус +$2000 и цикл
//    заново с ЕЗДЫ-ТУДА.
//
// Механика коробки — как в порту: attach модели в руку + анимация подъёма/укладки
// (freeze) + SpecialAction_Carry (ходьба с коробкой). Дополнительно к порту: за
// руль СВОЕГО грузовика С КОРОБКОЙ не пускаем (серверный driver-gate «запрет» —
// порт это не гейтил, см. Docs/PortHaulerJob.md).
//
// Окна фазозависимы: окно посадки/возврата за руль — ТОЛЬКО в фазах езды; в пеших
// фазах игрок легально пеший рядом с грузовиком (возврат-таймер не идёт). Анти-AFK:
// в езде — нет прогресса по чекпоинтам N с; в пеших — нет сданной коробки M с.
// Смерть/дисконнект/пропажа грузовика — увольнение (кошелёк неприкосновенен).
//
// Клиенту не доверяем: «за рулём своего грузовика» — серверный getDriver, вход в
// чекпоинт — принятая сервером позиция (CheckpointService), занятость площадки —
// серверный расчёт (anyVehicleNear), стейт «пеший» — PlayerStateService.
class HaulerJobSystem : public BaseSystem
{
  public:
    HaulerJobSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    // --- пикап + диалог ---
    void onPickup(IPlayer &player);
    void onStartWork(IPlayer &player);
    void onFinishWork(IPlayer &player);
    void onWithdrawMoney(IPlayer &player);
    void showInfo(IPlayer &player);

    // --- лайфцикл сессии ---
    void loadWallet(IPlayer &player, const PlayerSessionService::Session &session);
    void onSessionEnd(IPlayer &player);
    void onPlayerDeath(IPlayer &player);

    // --- гейт водителя (только руль) ---
    bool onDriverGate(IPlayer &player, IVehicle &vehicle);

    // --- резерв/очередь ---
    void onReserved(IPlayer &player);
    void pumpQueue();
    void notifyQueueShift();

    // --- маршрут (езда) ---
    void onDriveCheckpointEnter(IPlayer &player);
    void showDriveCheckpoint(IPlayer &player);
    // Переходы фаз (вызывают сервис + переставляют цель/сообщения/попап).
    void beginLoadingPhase(IPlayer &player);   // прибыл в порт: DriveOut -> Loading
    void beginDriveBackPhase(IPlayer &player); // погрузил 10: Loading -> DriveBack
    void beginUnloadingPhase(IPlayer &player); // приехал на базу: DriveBack -> Unloading
    void completeCyclePhase(IPlayer &player);  // разгрузил 10: Unloading -> DriveOut (заново)

    // --- пешая переноска коробки (погрузка/разгрузка) ---
    void onBoxCheckpointEnter(IPlayer &player);
    void onBoxPickup(IPlayer &player);      // вход в чекпоинт-источник: взять коробку
    void onLiftFinished(IPlayer &player);   // конец анимации подъёма: carry + чекпоинт-приёмник
    void onBoxDrop(IPlayer &player);        // вход в чекпоинт-приёмник: анимация укладки
    void onPutdownFinished(IPlayer &player); // конец укладки: снять коробку, зачесть
    void showBoxSource(IPlayer &player);    // чекпоинт, где БЕРУТ коробку (склад / зад грузовика)
    void showBoxDest(IPlayer &player);      // чекпоинт, куда КЛАДУТ (зад грузовика / выгрузка базы)
    void attachBox(IPlayer &player);
    void detachBox(IPlayer &player);

    // --- таймер депо (один общий, per-second) ---
    void onDepotTick();
    void restockDepot();
    void spawnPrestock(int spot);
    void tickActiveWorkers();
    void tickWorker(IPlayer &player);

    // --- увольнение/провал посадки ---
    void failBoarding(IPlayer &player);
    void dismiss(IPlayer &player, const std::string &reason, const Colour &colour);
    void teardownShift(IPlayer &player);

    // --- helpers ---
    bool drivingOwnTruck(int playerId) const; // за рулём СВОЕГО грузовика
    bool truckOnSpot(int vid, int spot) const;
    bool spotClear(int spot) const;
    // Позиция чекпоинта у ЗАДА грузовика игрока (от живой позиции/угла). false —
    // грузовик пропал (вызывающий увольняет). Пересчитывается на каждую коробку.
    bool backOfTruck(int playerId, Vector3 &out) const;
    Vector3 randomWarehousePoint(); // случайная точка склада (PortJobService)
    void clearCounters(int playerId);

    HaulerJobService &m_haulerJobService;
    HaulerWalletService &m_haulerWalletService;
    VehicleService &m_vehicleService;
    CheckpointService &m_checkpointService;
    PickupService &m_pickupService;
    PlayerMoneyService &m_moneyService;
    PlayerStateService &m_stateService;
    PlayerDialogService &m_dialogService;
    PlayerSessionService &m_sessionService;
    PlayerHealthService &m_healthService;
    TimerService &m_timers;
    ScreenNoticeService &m_screenNoticeService;
    ScreenTimerService &m_screenTimerService;
    MapIconService &m_mapIconService;
    VehicleWaypointService &m_waypointService;
    NavigationLockService &m_navLockService;
    PlayerAnimationService &m_animationService;
    AttachmentService &m_attachmentService;

    int m_pickup = -1;
    TimerService::Handle m_depotTimer;

    // Секундные счётчики окон (индексируются playerId; ведёт per-second тик):
    std::array<int, MAX_PLAYERS> m_reserveSeconds{};   // окно посадки (Reserved)
    std::array<int, MAX_PLAYERS> m_exitSeconds{};      // вне руля в фазе езды (окно возврата)
    std::array<int, MAX_PLAYERS> m_driveIdleSeconds{}; // за рулём без зачёта чекпоинта (анти-AFK езды)
    std::array<int, MAX_PLAYERS> m_footIdleSeconds{};  // пеший без сданной коробки (анти-AFK пеших фаз)

    // Слот AttachmentService текущей коробки в руке (-1 — нет). Отдельно от сервиса
    // (carrying-флаг там — авторитетная фаза; здесь — клиентская деталь attach).
    std::array<int, MAX_PLAYERS> m_boxSlot{};
    // Хэндл ожидающего таймера подъёма/укладки (отменяется на teardown — стале-колбэк
    // не должен продвинуть уже сброшенную/новую смену).
    std::array<TimerService::Handle, MAX_PLAYERS> m_pendingTimer{};

    std::mt19937 m_rng; // выбор случайной точки склада (главный поток, событийно)
};
