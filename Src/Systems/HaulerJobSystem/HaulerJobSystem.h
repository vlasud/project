#pragma once

#include "Macro.h"
#include "Services/HaulerJobService/HaulerJobService.h"
#include "Services/HaulerWalletService/HaulerWalletService.h"
#include "Services/JobDismissService/JobDismissService.h"
#include "Services/JobWalletService/JobWalletService.h"
#include "Services/Core/AttachmentService/AttachmentService.h"
#include "Services/Core/AudioService/AudioService.h"
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
    void showRoleChoice(IPlayer &player); // «Начать работу» -> водитель или грузчик
    void startAsDriver(IPlayer &player);
    void startAsLoader(IPlayer &player);
    void onFinishWork(IPlayer &player);
    void onWithdrawMoney(IPlayer &player);
    void showInfo(IPlayer &player);

    // --- пара «водитель + грузчик» ---
    void onPairCommand(IPlayer &driver, int targetId); // /pair — приглашение
    void onPairAccepted(int driverId, int loaderId);   // грузчик принял диалог
    void onUnpairCommand(IPlayer &player);             // /unpair — разойтись, работу не теряя
    // Вернуть игроку цель коробки, когда он снова стал носильщиком смены.
    void restoreCarrierTarget(IPlayer &player);
    // Развести пару: обе стороны узнают, носильщик меняется. Зовётся из teardown
    // любой из сторон (выход/смерть/увольнение) и при ручном разрыве.
    void splitPair(int playerId, const std::string &noticeForPartner);
    // Снять с игрока всё «носильщицкое» (коробка, анимация, carry, чекпоинт).
    void clearCarryState(IPlayer &player);

    // --- лайфцикл сессии ---
    void loadWallet(IPlayer &player, const PlayerSessionService::Session &session);
    void onSessionEnd(IPlayer &player);
    void onPlayerDeath(IPlayer &player);

    // --- гейт водителя (только руль) ---
    bool onDriverGate(IPlayer &player, IVehicle &vehicle);

    // --- резерв/очередь ---
    void onReserved(IPlayer &player);
    // Сел за руль своего грузовика: Reserved -> DriveOut, окно посадки закрыто,
    // маркер грузовика снят, показан первый чекпоинт маршрута.
    void completeBoarding(IPlayer &player);
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
    // Освободить площадки, с которых грузовик уже уехал (или пропал): pre-stock в депо
    // нет, площадку держит машина конкретного работника.
    void releaseDepartedSpots();
    // Подать работнику грузовик на его площадку. false — не смогли (пул полон).
    bool spawnForWorker(IPlayer &player);
    void tickActiveWorkers();
    void tickWorker(IPlayer &player);
    void tickLoader(IPlayer &player); // грузчик: своей фазы нет, живёт фазой напарника
    // Цель «взять коробку» на разгрузке следует за ЖИВЫМ грузовиком: в кабине её нет,
    // на выходе ставится от текущей позиции. Общая для соло-водителя и грузчика.
    void refreshUnloadSource(IPlayer &carrier);

    // --- увольнение/провал посадки ---
    void failBoarding(IPlayer &player);
    void dismiss(IPlayer &player, const std::string &reason, const Colour &colour);
    void dismissShiftOwner(int ownerId, const std::string &reason); // уволить водителя смены по id
    void teardownShift(IPlayer &player);

    // --- helpers ---
    bool drivingOwnTruck(int playerId) const; // за рулём СВОЕГО грузовика
    bool truckOnSpot(int vid, int spot) const;
    bool spotClear(int spot) const;
    // Позиция чекпоинта у ЗАДА грузовика СМЕНЫ (от живой позиции/угла). Принимает id
    // ВОДИТЕЛЯ смены (у грузчика своего грузовика нет). false — грузовик пропал.
    bool backOfTruck(int shiftOwnerId, Vector3 &out) const;
    // Начислить участнику плату за коробку/бонус: кошелёк (write-through) + попап.
    void creditParticipant(int playerId, std::int64_t amount, const std::string &popup, Milliseconds popupTime);
    // Случайная точка склада (PortJobService), не ближе MIN_CARRY_DISTANCE к awayFrom
    // (заду грузовика) — коробку всегда нужно нести.
    Vector3 randomWarehousePoint(const Vector3 &awayFrom);
    void clearCounters(int playerId);

    HaulerJobService &m_haulerJobService;
    HaulerWalletService &m_haulerWalletService;
    JobWalletService &m_jobWalletService;
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
    AudioService &m_audioService;

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
