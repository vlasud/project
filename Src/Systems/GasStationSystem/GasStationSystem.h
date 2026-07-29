#pragma once

#include "Services/BusinessService/BusinessService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "Systems/BusinessShop/BusinessShop.h"
#include "player.hpp"

// АЗС — ТИП бизнеса. Внутри — та же витрина, что у 24/7 (общий BusinessShop);
// снаружи — колонки: сидя за рулём рядом с точкой можно залить бак за деньги.
//
// Как и 24/7, регистрирует себя в BusinessService (имя, пул интерьеров, меню
// посетителя) — общий привод про АЗС не знает ничего.
//
// ЗАПРАВКА РАБОТАЕТ ВСЕГДА, даже у ничейной точки: это инфраструктура мира, без
// неё машины встают намертво. Владение решает лишь, КОМУ идёт выручка — у
// ничейной АЗС деньги просто уходят из экономики (сток), в копилку класть некому.
//
// Топливо — VehicleService (серверная правда о баке); сюда оно приходит только
// через refuel, своего учёта бензина здесь нет.
class GasStationSystem : public BaseSystem
{
  public:
    GasStationSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Ближайшая АЗС к точке в радиусе обслуживания; -1 — рядом ничего нет.
    int stationNear(const Vector3 &position) const;

    // /fuel: диалог заправки для водителя рядом с колонкой.
    void showFuelMenu(IPlayer &player);
    void showLitresInput(IPlayer &player, int businessId);
    // Залить litres литров (кламп по свободному месту в баке) с оплатой.
    void refuel(IPlayer &player, int businessId, float litres);

    BusinessService &m_businessService;
    PlayerDialogService &m_dialogService;
    PlayerLocationService &m_locationService;
    PlayerStateService &m_stateService;
    PlayerMoneyService &m_moneyService;
    VehicleService &m_vehicleService;
    PlayerSessionService &m_sessionService; // ключ аккаунта заправляющегося
    BusinessShop m_shop;
};
