#include "Systems/VehicleLockSystem/VehicleLockSystem.h"

#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"

VehicleLockSystem::VehicleLockSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_lockService(serviceRegister.getService<VehicleLockService>())
{
    m_lockService.bind(serviceRegister.getService<VehicleService>(),
                       serviceRegister.getService<PersonalVehicleService>(),
                       serviceRegister.getService<ParkedVehicleService>(),
                       serviceRegister.getService<FamilyService>(),
                       serviceRegister.getService<PlayerSessionService>(), core);

    VehicleService &vehicleService = serviceRegister.getService<VehicleService>();
    vehicleService.subscribeStreamedInForPlayer([this](IVehicle &vehicle, IPlayer &player)
                                                { m_lockService.onStreamedInForPlayer(vehicle, player); });
    vehicleService.subscribeDestroyed([this](IVehicle &vehicle) { m_lockService.onDestroyed(vehicle.getID()); });

    serviceRegister.getService<ParkedVehicleService>().subscribeReconcile(
        [this](long long dbId) { m_lockService.onReconcile(dbId); });
}
