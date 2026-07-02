#pragma once

#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Vehicles/vehicles.hpp>

// Привод сервиса машин: смены стейта привязывают/отвязывают пассажира, апдейт
// водителя сверяет HP машины, unoccupied/trailer sync валидируются и при фейке
// отклоняются (ядро их не применяет), нарушения уходят в журнал античита.
// Секундный таймер гонит secondTick (дренаж топлива + тушение машин без
// водителя); смерть машины без серверной санкции возвращает машину целой
// отложенным респавном, БЕЗ записи нарушения (контракт смерти — VehicleService.h).
class VehicleSystem : public BaseSystem,
                      public PlayerChangeEventHandler,
                      public PlayerUpdateEventHandler,
                      public PlayerConnectEventHandler,
                      public PlayerShotEventHandler,
                      public VehicleEventHandler,
                      public PoolEventHandler<IVehicle>
{
  public:
    VehicleSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerStateChange(IPlayer &player, PlayerState newState, PlayerState oldState) override;
    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;
    bool onPlayerShotVehicle(IPlayer &player, IVehicle &target, const PlayerBulletData &bulletData) override;

    void onVehicleSpawn(IVehicle &vehicle) override;
    void onVehicleDeath(IVehicle &vehicle, IPlayer &player) override;
    void onPlayerEnterVehicle(IPlayer &player, IVehicle &vehicle, bool passenger) override;
    bool onVehicleMod(IPlayer &player, IVehicle &vehicle, int component) override;
    bool onVehiclePaintJob(IPlayer &player, IVehicle &vehicle, int paintJob) override;
    bool onVehicleRespray(IPlayer &player, IVehicle &vehicle, int colour1, int colour2) override;
    void onEnterExitModShop(IPlayer &player, bool enterexit, int interiorID) override;
    bool onUnoccupiedVehicleUpdate(IVehicle &vehicle, IPlayer &player, UnoccupiedVehicleUpdate const updateData) override;
    bool onTrailerUpdate(IPlayer &player, IVehicle &trailer) override;

    void onPoolEntryCreated(IVehicle &vehicle) override;
    void onPoolEntryDestroyed(IVehicle &vehicle) override;

  private:
    void record(IPlayer &player, VehicleService::Outcome &outcome);

    VehicleService &m_vehicleService;
    PlayerStateService &m_stateService;
    AntiCheatService &m_antiCheatService;
    TimerService &m_timerService;
};
