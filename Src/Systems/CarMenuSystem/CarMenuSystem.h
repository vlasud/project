#pragma once

#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Меню личного транспорта (/car) — бизнес-фича (НЕ Core). Дистанционный близнец
// пикапа парковки: тот же источник владения (PersonalVehicleService::owned), тот же
// красный чекпоинт-указатель (через общий VehicleWaypointService), но вызывается
// откуда угодно по карте.
//
// Петля: /car -> LIST-диалог со списком ЛИЧНЫХ машин игрока -> выбор машины ->
// ПОД-ДИАЛОГ действий по ней (LIST — задел на рост, пунктов будет больше) ->
// первое (пока единственное) действие «Показать на карте» = чекпоинт-указатель к
// машине. Событийный (команда/диалог), per-tick работы нет.
//
// Игрок видит и действует ТОЛЬКО над своими машинами (владение серверное по
// playerId); индекс машины ре-валидируется против актуального owned на каждом шаге;
// указатель раскрывает позицию только СВОЕЙ машины.
class CarMenuSystem : public BaseSystem
{
  public:
    CarMenuSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // /car: нет машин -> сообщение; иначе LIST-диалог выбора машины.
    void showCarList(IPlayer &player);
    // Под-диалог действий по машине carIndex (валидный на момент показа).
    void showCarActions(IPlayer &player, int carIndex);
    // Действие «Показать на карте» для машины carIndex.
    void showOnMap(IPlayer &player, int carIndex);

    PersonalVehicleService &m_personalService;
    VehicleService &m_vehicleService;
    VehicleWaypointService &m_waypointService;
    PlayerDialogService &m_dialogService;
};
