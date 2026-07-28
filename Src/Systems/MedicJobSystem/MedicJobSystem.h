#pragma once

#include "Macro.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/NavigationLockService/NavigationLockService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerSkinService/PlayerSkinService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/Core/ScreenTimerService/ScreenTimerService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/JobDismissService/JobDismissService.h"
#include "Services/JobWalletService/JobWalletService.h"
#include "Services/MedicJobService/MedicJobService.h"
#include "Services/MedicWalletService/MedicWalletService.h"
#include "Services/PlayerPersonalSkinService/PlayerPersonalSkinService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <random>

// Работа-врач (больница) — привод MedicJobService. Бизнес-фича, НЕ Core.
//
// Отличия от работ с маршрутом (автобус, развозчик): маршрута нет вовсе, есть только
// выдача машины и лечение. Поэтому нет ни чекпоинтов, ни окна возврата за руль —
// врач ЛЕГАЛЬНО ходит пешком у своей скорой, в этом и состоит работа.
//
//  * Устройство у пикапа больницы -> очередь на одну из SPOT_COUNT точек спавна.
//  * Дошла очередь -> на точке появляется ЛИЧНАЯ скорая работника + красный маркер
//    на неё + окно BOARDING_SECONDS. Точка занята, пока машина на ней стоит, поэтому
//    окно закрывает не посадка, а ОТЪЕЗД: не уехал вовремя -> машину снимают, работник
//    в конец очереди (иначе один игрок держал бы точку всю смену).
//  * На устройстве выдаётся форменный скин из пула по полу аккаунта (сессия знает
//    пол; PlayerSkinService::setSkin — БАЗА, иначе первая смерть откатила бы форму).
//    На увольнении возвращается личный скин (PlayerPersonalSkinService).
//  * /med [id] лечит игрока рядом со скорой; за каждого — деньги в персистентный
//    кошелёк врача (MedicWalletService, write-through), забираются на пикапе.
//
// Клиенту не доверяем: «за рулём своей скорой» — серверный getDriver, дистанции —
// принятые сервером позиции (PlayerLocationService), стейт «пеший» —
// PlayerStateService, HP пациента — PlayerHealthService.
class MedicJobSystem : public BaseSystem
{
  public:
    MedicJobSystem(ICore &core, const ServiceRegister &serviceRegister);

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

    // --- очередь и выдача машины ---
    void onSpotGranted(IPlayer &player); // точка досталась: спавн машины + окно
    void pumpQueue();
    void notifyQueueShift();
    void failBoarding(IPlayer &player); // не отъехал вовремя -> в конец очереди

    // --- лечение ---
    void onMedCommand(IPlayer &medic, int targetId);

    // --- форма ---
    void applyUniform(IPlayer &player);
    void restoreOwnSkin(IPlayer &player);

    // --- таймер больницы (один общий, per-second) ---
    void onHospitalTick();
    void tickWorker(IPlayer &player);

    // --- увольнение ---
    void dismiss(IPlayer &player, const std::string &reason, const Colour &colour);
    void teardownShift(IPlayer &player);

    // --- helpers ---
    // Машина всё ещё стоит на своей точке спавна (значит, точка занята).
    bool ambulanceOnSpot(int vehicleId, int spot) const;
    // Позиция скорой врача; false — машина пропала.
    bool ambulancePosition(int playerId, Vector3 &out) const;

    MedicJobService &m_medicJobService;
    MedicWalletService &m_medicWalletService;
    VehicleService &m_vehicleService;
    PickupService &m_pickupService;
    PlayerMoneyService &m_moneyService;
    PlayerStateService &m_stateService;
    PlayerDialogService &m_dialogService;
    PlayerSessionService &m_sessionService;
    PlayerHealthService &m_healthService;
    PlayerLocationService &m_locationService;
    PlayerSkinService &m_skinService;
    PlayerPersonalSkinService &m_personalSkinService;
    TimerService &m_timers;
    ScreenNoticeService &m_screenNoticeService;
    ScreenTimerService &m_screenTimerService;
    MapIconService &m_mapIconService;
    VehicleWaypointService &m_waypointService;
    NavigationLockService &m_navLockService;
    JobWalletService &m_jobWalletService;

    int m_pickup = -1;
    TimerService::Handle m_hospitalTimer;

    // Секунды окна посадки (индексируется playerId; ведёт per-second тик).
    std::array<int, MAX_PLAYERS> m_boardingSeconds{};

    std::mt19937 m_rng; // выбор скина из пула (главный поток, событийно)
};
