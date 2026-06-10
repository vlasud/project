#pragma once

#include "Macro.h"
#include "Services/Core/GridService/GridService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Vehicles/vehicles.hpp>
#include <array>

// Ведёт динамические сущности в пространственной сетке:
//  * игроки — добавляются при спавне, позиция обновляется на каждом апдейте
//    (внутри сетки это дёшево: чаще всего перезапись трёх float в той же ячейке);
//  * машины — добавляются/убираются по событиям пула, позиция обновляется
//    апдейтом водителя (машина без водителя сама не ездит).
// Статику (объекты, пикапы, иконки) регистрируют их собственные системы
// напрямую через GridService.
class GridSystem : public BaseSystem,
                   public PlayerUpdateEventHandler,
                   public PlayerSpawnEventHandler,
                   public PlayerConnectEventHandler,
                   public PoolEventHandler<IVehicle>
{
  public:
    GridSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

    void onPoolEntryCreated(IVehicle &vehicle) override;
    void onPoolEntryDestroyed(IVehicle &vehicle) override;

  private:
    GridService &m_gridService;
    PlayerLocationService &m_locationService; // позиция игрока — из источника правды
    PlayerStateService &m_stateService;       // стейт — из источника истины
    VehicleService &m_vehicleService;         // машина игрока — из источника правды
    IVehiclesComponent *m_vehicles = nullptr;

    std::array<GridService::Handle, MAX_PLAYERS> m_playerHandles;
    std::array<GridService::Handle, VEHICLE_POOL_SIZE> m_vehicleHandles;
};
