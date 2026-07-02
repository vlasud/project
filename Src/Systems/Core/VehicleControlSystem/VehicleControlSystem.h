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
//
// Тексты отказа (стол/пустой бак) — Docs/GameDesign/UI_Texts.md, «Неубиваемость
// машин»/«Отказ завести двигатель». VehicleService::setEngine сам текста не шлёт
// (Core) — здесь спрашивается ПРИЧИНА отказа (isStalled/isOutOfFuel) ДО вызова, и
// сообщение шлётся только на попытке ЗАВЕСТИ (а не заглушить); антиспам — фронт
// клавиши (PlayerKeyService), строка раз на нажатие. subscribeFuelEmpty —
// подписка на момент опустошения бака ПОД ВОДИТЕЛЕМ (secondTick VehicleService,
// не per-tick): Core оповещает фактом, текст шлёт эта система.
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
