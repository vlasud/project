#pragma once

#include "Macro.h"
#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/IService.h"
#include "Server/Components/Vehicles/vehicles.hpp"
#include "player.hpp"
#include <array>

// Указатель-на-машину — бизнес-фича (НЕ Core): единый владелец персонального
// красного чекпоинта, ведущего игрока к ЕГО машине. Один слот на игрока (как у
// CheckpointService::setForPlayer — клиент показывает только один обычный
// чекпоинт), поэтому пере-вызов заменяет прежний указатель.
//
// Переиспользуется парковкой (подача машины ставит указатель к ней) и меню /car
// («Показать на карте»): раньше бухгалтерию чекпоинта вела ParkingSystem у себя —
// теперь она здесь, единая. Сервис не подписывается на события сам; его «привод»
// (VehicleWaypointSystem) снимает указатель на уничтожении машины-цели и на
// дисконнекте, как CheckpointSystem обслуживает CheckpointService.
//
// Событийный (подача/команда/уничтожение), per-tick работы нет. bounds playerId
// везде; чекпоинт ставится только при связанном CheckpointService (bind).
class VehicleWaypointService final : public IService
{
  public:
    // Радиус красного чекпоинта у машины: стандартный наземный (в ряд со входами/
    // работами), как был у парковки.
    static constexpr float CHECKPOINT_RADIUS = 3.0f;

    // Привязать CheckpointService (владелец клиентского чекпоинт-слота). Зовётся
    // VehicleWaypointSystem в конструкторе — реестр конструирует сервисы дефолтным
    // ctor, зависимость проставляется здесь (как VehicleService::bind).
    void bind(CheckpointService &checkpoints);

    // Поставить игроку красный чекпоинт-указатель к машине (её текущая позиция).
    // Пере-вызов заменяет прежний (setForPlayer перерисует слот). Вход в чекпоинт
    // снимает его сам (onEnter -> clearFor). Bounds-safe; no-op без bind.
    void showFor(IPlayer &player, IVehicle &vehicle);

    // Снять указатель игрока (если стоит) + гасит цель. Идемпотентно, bounds-safe.
    void clearFor(IPlayer &player);

    // Обнулить цель слота БЕЗ обращения к CheckpointService — на дисконнекте
    // (клиентский чекпоинт CheckpointService сбросит сам своим resetPlayer).
    // Bounds-safe.
    void resetPlayer(int playerId);

    // id игрока, чей указатель ведёт к этой машине (или -1). Для снятия указателя
    // на уничтожении машины-цели. vehicleId == -1 -> -1. Bounds-safe, линейный.
    int findByVehicle(int vehicleId) const;

    // Стоит ли у игрока указатель. Bounds-safe (false для невалидного id).
    bool hasWaypoint(int playerId) const;

  private:
    CheckpointService *m_checkpoints = nullptr; // владелец клиентского чекпоинт-слота (bind)

    // Per-player целевой vehicleId указателя: -1 — указателя нет.
    struct Target
    {
        int vehicleId = -1;
    };
    std::array<Target, MAX_PLAYERS> m_targets{};
};
