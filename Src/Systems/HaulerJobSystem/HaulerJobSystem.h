#pragma once

#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Systems/BaseSystem.h"

namespace
{
    const std::array TRUCK_SPAWN_POSITIONS =
    {
        Vector3{2167.6926f, -2275.5762f, 13.5707f},
        Vector3{2175.4233f,-2268.4973f,13.6202f},
        Vector3{2161.5037,-2282.0056,13.6142},
        Vector3{2155.2537,-2289.8650,13.6094}
    };
    constexpr float TRUCK_SPAWN_ROTATION = 225.0f;
    constexpr int TRUCK_MODEL = 456;
    const Vector3 PICKUP_POSITION = {2171.0681,-2252.5254,13.3026};

}

class HaulerJobSystem final : BaseSystem
{
public:
    explicit HaulerJobSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

private:

    PickupService& m_pickupService;
    VehicleService& m_vehicleService;
    PlayerDialogService& m_dialogService;
};
