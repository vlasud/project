#pragma once

#include "../../Services/GridService/GridService.h"
#include "../../Services/PlayerLocationService/PlayerLocationService.h"
#include "../../Services/PlayerStateService/PlayerStateService.h"
#include "../../Services/VehicleService/VehicleService.h"
#include "../BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Vehicles/vehicles.hpp>

// Отладка машин и связанных сервисов:
//  /veh [модель]   — заспавнить машину рядом и сесть (через putInVehicle — санкция);
//  /vput           — сесть в ближайшую машину (поиск через сетку + санкция);
//  /vdel           — удалить машину, в которой сидишь;
//  /vrespawn       — переспавнить машину, в которой сидишь;
//  /vinfo          — серверное HP vs клиентское, место, id, расхождение;
//  /vhp [hp]       — серверное HP через сервис;
//  /vrepair        — серверный ремонт через сервис;
//  /vengine, /vlock — двигатель/замки через сервис;
//  /vhack          — СИМУЛЯЦИЯ repair hack: сырой setHealth(1000) мимо сервиса —
//                    валидатор должен откатить и записать нарушение.
class VehicleDebugSystem : public BaseSystem
{
  public:
    VehicleDebugSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    IVehicle *currentVehicle(IPlayer &player); // машина игрока или сообщение об ошибке

    VehicleService &m_vehicleService;
    PlayerStateService &m_stateService;
    PlayerLocationService &m_locationService;
    GridService &m_gridService;

    IVehiclesComponent *m_vehicles = nullptr;
};
