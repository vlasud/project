#include "HaulerJobSystem.h"

namespace
{

}

HaulerJobSystem::HaulerJobSystem(ICore &core, const ServiceRegister &serviceRegister) :
    BaseSystem(core, serviceRegister),
    m_pickupService(serviceRegister.getService<PickupService>()),
    m_vehicleService(serviceRegister.getService<VehicleService>()),
    m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
}

void HaulerJobSystem::initialize(IComponentList *components)
{
    for (auto i : TRUCK_SPAWN_POSITIONS)
    {
        m_vehicleService.create(TRUCK_MODEL, i, TRUCK_SPAWN_ROTATION, 0, 0, VehicleService::Owner::Work, 0);
    }

    //m_pickupService.add()
}
