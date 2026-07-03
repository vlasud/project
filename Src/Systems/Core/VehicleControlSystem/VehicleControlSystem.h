#pragma once

#include "Services/Core/PlayerKeyService/PlayerKeyService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
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
//  * двигатель — Action (бит 1): в машине этот бит ставит левый Ctrl (и ALT GR / NUM0);
//  * фары — Fire (бит 4): в машине это ЛКМ и левый Alt (drive-by fire).
//
// Тексты отказа (стол/пустой бак) — Docs/GameDesign/UI_Texts.md, «Неубиваемость
// машин»/«Отказ завести двигатель». VehicleService::setEngine сам текста не шлёт
// (Core) — здесь спрашивается ПРИЧИНА отказа (isStalled/isOutOfFuel) ДО вызова, и
// сообщение шлётся только на попытке ЗАВЕСТИ (а не заглушить); антиспам — фронт
// клавиши (PlayerKeyService), строка раз на нажатие. Поломка двигателя (стол) —
// заметный красный попап через единый ScreenNoticeService (не native GameText —
// см. Docs/ScreenNotice.md), а НЕ чат: и на попытку завести сломанный, и на сам
// момент поломки (subscribeEngineBroken). subscribeFuelEmpty — момент опустошения
// бака ПОД ВОДИТЕЛЕМ (secondTick VehicleService, не per-tick): Core оповещает
// фактом, текст шлёт эта система.
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

    // Попап «двигатель сломан» водителю: и на попытку завести сломанный
    // двигатель, и на сам момент поломки.
    void showEngineBroken(IPlayer &player);
    // Попап «пустой бак» водителю: и на попытку завести с пустым баком, и на сам
    // момент опустошения бака на ходу.
    void showNoFuel(IPlayer &player);

    PlayerKeyService &m_keyService;
    VehicleService &m_vehicleService;
    ScreenNoticeService &m_screenNotice;
};
