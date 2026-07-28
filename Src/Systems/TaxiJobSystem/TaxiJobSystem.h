#pragma once

#include "Macro.h"
#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/NavigationLockService/NavigationLockService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/Core/ScreenTimerService/ScreenTimerService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/JobDismissService/JobDismissService.h"
#include "Services/PlaceCatalogService/PlaceCatalogService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/TaxiJobService/TaxiJobService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>

// Работа-таксист — привод TaxiJobService. Бизнес-фича, НЕ Core.
//
// Депо как у врача: pre-stock машин нет, точка спавна пустует до очереди, окно выезда
// закрывает ОТЪЕЗД с точки, а не посадка за руль.
//
// ПОЕЗДКА — договор двух игроков, поэтому вся она построена на согласии обеих сторон
// и на серверных фактах:
//   1. пассажир сел в такси -> ему в чат «введите /taxi»;
//   2. /taxi (пассажир): выбрать место из общего каталога ЛИБО указать меткой на
//      карте -> водителю встаёт чекпоинт туда;
//   3. /taxipay <сумма> (водитель): проверка, что у пассажира столько есть ->
//      пассажиру диалог «согласиться/отказаться»;
//   4. согласился -> деньги списываются СРАЗУ, но лежат ДЕПОЗИТОМ в поездке;
//   5. такси доехало до чекпоинта с пассажиром на борту -> депозит уходит водителю.
//
// Любой другой конец поездки ВОЗВРАЩАЕТ депозит пассажиру: вышел водитель, погиб,
// уволился, пропала машина, пассажир вышел из такси или из игры. Единственная точка
// закрытия — closeRide, поэтому «забыть» про висящие деньги неоткуда.
//
// Клиенту не доверяем: «за рулём своего такси» и «пассажир на борту» — серверные
// getDriver/getVehicle, вход в чекпоинт — принятая сервером позиция (CheckpointService),
// баланс пассажира — PlayerMoneyService.
class TaxiJobSystem : public BaseSystem, public PlayerClickEventHandler
{
  public:
    TaxiJobSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    // Метка на карте от пассажира — точка назначения поездки.
    void onPlayerClickMap(IPlayer &player, Vector3 pos) override;

  private:
    // --- пикап + диалог ---
    void onPickup(IPlayer &player);
    void onStartWork(IPlayer &player);
    void onFinishWork(IPlayer &player);
    void showInfo(IPlayer &player);

    // --- лайфцикл сессии ---
    void onSessionEnd(IPlayer &player);
    void onPlayerDeath(IPlayer &player);

    // --- гейт водителя ---
    bool onDriverGate(IPlayer &player, IVehicle &vehicle);

    // --- очередь и выдача машины ---
    void onSpotGranted(IPlayer &player);
    void pumpQueue();
    void notifyQueueShift();
    void failBoarding(IPlayer &player);

    // --- поездка ---
    void onTaxiCommand(IPlayer &passenger);          // /taxi — выбор назначения
    void showPlaceChoice(IPlayer &passenger);        // список мест каталога
    void awaitMapClick(IPlayer &passenger);          // «указать на карте»
    void applyDestination(int driverId, int passengerId, const Vector3 &destination, const std::string &name);
    void onTaxiPayCommand(IPlayer &driver, int fare); // /taxipay — предложить цену
    void onFareAccepted(int driverId, int passengerId);
    void onDestinationReached(IPlayer &driver);
    // Единственная точка закрытия поездки: платит водителю (delivered) либо возвращает
    // депозит пассажиру, гасит чекпоинт и чистит запись.
    void closeRide(int driverId, bool delivered, const std::string &reasonForPassenger);
    // Пассажир всё ещё в такси этого водителя (серверный факт).
    bool passengerAboard(int driverId) const;

    // --- таймер таксопарка (один общий, per-second) ---
    void onDepotTick();
    void tickWorker(IPlayer &player);

    // --- увольнение ---
    void dismiss(IPlayer &player, const std::string &reason, const Colour &colour);
    void teardownShift(IPlayer &player);

    // --- helpers ---
    // Водитель за рулём СВОЕГО такси (серверный getDriver).
    bool drivingOwnTaxi(int playerId) const;
    // На точке спавна физически пусто (чужой машины нет) — можно подавать.
    bool spotClear(int spot) const;
    bool taxiOnSpot(int vehicleId, int spot) const;

    TaxiJobService &m_taxiJobService;
    PlaceCatalogService &m_placeCatalogService;
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

    int m_pickup = -1;
    TimerService::Handle m_depotTimer;

    // Секунды окна выезда (индексируется playerId; ведёт per-second тик).
    std::array<int, MAX_PLAYERS> m_boardingSeconds{};
    // Секунды вне руля своего такси на смене — окно возврата.
    std::array<int, MAX_PLAYERS> m_awaySeconds{};
    // Пассажир ждёт клика по карте (после «Указать на карте»): без этого флага любая
    // метка любого игрока трактовалась бы как заказ такси.
    std::array<bool, MAX_PLAYERS> m_awaitingMapClick{};
};
