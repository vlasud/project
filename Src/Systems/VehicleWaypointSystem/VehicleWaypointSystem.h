#pragma once

#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Привод указателя-на-машину: связывает VehicleWaypointService с CheckpointService
// (bind), снимает указатель на уничтожении машины-цели и чистит слот на
// коннекте/дисконнекте. Как CheckpointSystem для CheckpointService — без команд и
// без бизнес-логики (её несут ParkingSystem и CarMenuSystem, ставя указатель).
class VehicleWaypointSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    VehicleWaypointSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    VehicleWaypointService &m_waypointService;
    VehicleService &m_vehicleService;
};
