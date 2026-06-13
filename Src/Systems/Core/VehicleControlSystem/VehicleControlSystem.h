#pragma once

#include "Services/Core/PlayerKeyService/PlayerKeyService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Базовые билды управления машиной для ВОДИТЕЛЯ: тоггл двигателя и фар по
// нажатию клавиш. Системе ничего не приходит per-tick — она лишь подписывается
// в PlayerKeyService на фронт нужных клавиш, а гейтинг «это его машина» делает
// по серверному стейту VehicleService.
//
// Маппинг клавиш (см. Docs/VehicleControls.md). За рулём GTA:SA шлёт другой набор
// бит, чем на ногах, и левый Ctrl с ЛКМ в машине — разные биты:
//  * двигатель — Fire (бит 4): в машине это левый Ctrl (drive-by fire);
//  * фары — Action (бит 1): в машине это ЛКМ.
class VehicleControlSystem : public BaseSystem
{
  public:
    VehicleControlSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Тоггл двигателя машины, водителем которой игрок реально является.
    void toggleEngine(IPlayer &player);
    // Тоггл фар той же машины.
    void toggleLights(IPlayer &player);

    // Машина игрока, только если он её ВОДИТЕЛЬ (seat 0); иначе nullptr.
    IVehicle *drivenVehicle(IPlayer &player) const;

    PlayerKeyService &m_keyService;
    VehicleService &m_vehicleService;
};
