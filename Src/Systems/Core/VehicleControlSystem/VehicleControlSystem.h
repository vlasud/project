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
// Маппинг клавиш (см. Docs/VehicleControls.md):
//  * двигатель — Fire (бит 4): по умолчанию это и левый Ctrl, и ЛКМ — серверно
//    они один бит и неразличимы, поэтому оба физических нажатия глушат/заводят;
//  * фары — Crouch (бит 2): отдельный свободный бит, т.к. фары нельзя повесить на
//    тот же Fire, что и двигатель (вынужденный выбор взамен ЛКМ).
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
