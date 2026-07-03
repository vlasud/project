#pragma once

#include "Services/VehicleLockService/VehicleLockService.h"
#include "Systems/BaseSystem.h"

// Привод бизнес-фичи «Замок дверей личного транспорта» (НЕ Core): в конструкторе
// связывает VehicleLockService с зависимостями (bind) и подписывается на:
//  - VehicleService::subscribeStreamedInForPlayer — ядро не восстанавливает
//    пер-игровые params на стрим-ине, сервис переприменяет замок для этого игрока;
//  - VehicleService::subscribeDestroyed — экземпляр уничтожен -> забыть запись
//    замка (новая жизнь машины на этом vehicleId открыта по умолчанию);
//  - ParkedVehicleService::subscribeReconcile — флип family_id (share/unshare)
//    меняет состав «свой» для уже закрытой машины -> переприменить всем.
// Само чтение/переключение замка — CarMenuSystem (через VehicleLockService API),
// эта система только держит подписки. Событийная, per-tick работы нет.
class VehicleLockSystem : public BaseSystem
{
  public:
    VehicleLockSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    VehicleLockService &m_lockService;
};
