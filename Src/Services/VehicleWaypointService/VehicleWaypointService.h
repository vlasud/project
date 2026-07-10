#pragma once

#include "Macro.h"
#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/IService.h"
#include "Server/Components/Vehicles/vehicles.hpp"
#include "player.hpp"
#include <array>
#include <functional>

// Указатель-на-машину — бизнес-фича (НЕ Core): единый владелец персонального
// красного чекпоинта, ведущего игрока к ЕГО машине ЛИБО к статической точке
// (например, входу дома). Один слот на игрока (как у CheckpointService::
// setForPlayer — клиент показывает только один обычный чекпоинт), поэтому
// пере-вызов заменяет прежний указатель.
//
// Переиспользуется парковкой (подача машины ставит указатель к ней), меню /car
// («Показать на карте») и меню /home («Отметить на карте» — точка входа дома):
// раньше бухгалтерию чекпоинта вела ParkingSystem у себя — теперь она здесь,
// единая. Сервис не подписывается на события сам; его «привод»
// (VehicleWaypointSystem) снимает указатель на уничтожении машины-цели и на
// дисконнекте, как CheckpointSystem обслуживает CheckpointService.
//
// ДВА РЕЖИМА ЦЕЛИ (единый слот, общий CheckpointService::setForPlayer):
//  * vehicleId >= 0 — указатель на машину. findByVehicle/автосъём на уничтожении
//    машины-цели (VehicleWaypointSystem::subscribeDestroyed) работают ТОЛЬКО для
//    этого режима.
//  * vehicleId == -1 (статическая точка, showPoint) — указатель на фиксированные
//    координаты. У такой цели нет живого экземпляра, который могут уничтожить,
//    поэтому findByVehicle её никогда не матчит (не спутать с «нет цели») и
//    привод её не трогает — снимается только явным clearFor/сменой цели/
//    дисконнектом (resetPlayer). ВАЖНО: клиентский чекпоинт-слот — ЕДИНЫЙ ресурс
//    на игрока; заводить отдельного «владельца» точки в другой системе нельзя —
//    два хозяина слота начали бы гасить чекпоинты друг друга.
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

    // Поставить игроку красный чекпоинт-указатель к СТАТИЧЕСКОЙ точке (не к
    // машине) — например, входу дома. Тот же единый слот: пере-вызов (в т.ч.
    // showFor) заменяет цель. Вход в чекпоинт снимает его сам (onEnter -> clearFor).
    // Bounds-safe; no-op без bind.
    void showFor(IPlayer &player, const Vector3 &point);

    // Поставить GPS-указатель (/gps) к СТАТИЧЕСКОЙ точке. Тот же единый слот, но цель
    // помечается как GPS (hasGpsWaypoint/clearGpsFor различают её от парковочного/
    // домашнего указателя). onArrive вызывается при входе в чекпоинт ПОСЛЕ снятия
    // маркера (clearFor уже отработал) — для сообщения о прибытии. Пере-вызов (в т.ч.
    // showFor к машине/точке) заменяет цель и снимает пометку GPS. Bounds-safe; no-op
    // без bind.
    void showGpsFor(IPlayer &player, const Vector3 &point, std::function<void(IPlayer &)> onArrive);

    // Снять указатель игрока (если стоит) + гасит цель. Идемпотентно, bounds-safe.
    void clearFor(IPlayer &player);

    // Снять указатель ТОЛЬКО если текущая цель — GPS (иначе no-op): для гашения GPS
    // на взятии лока навигации, не трогая парковочный/домашний указатель.
    // Идемпотентно, bounds-safe.
    void clearGpsFor(IPlayer &player);

    // Стоит ли у игрока именно GPS-указатель (не парковка/дом). Bounds-safe.
    bool hasGpsWaypoint(int playerId) const;

    // Обнулить цель слота БЕЗ обращения к CheckpointService — на дисконнекте
    // (клиентский чекпоинт CheckpointService сбросит сам своим resetPlayer).
    // Bounds-safe.
    void resetPlayer(int playerId);

    // id игрока, чей указатель ведёт к этой машине (или -1). Для снятия указателя
    // на уничтожении машины-цели. vehicleId == -1 -> -1 ВСЕГДА (не матчит цели-точки
    // — у них нет живого экземпляра, который можно уничтожить). Bounds-safe, линейный.
    int findByVehicle(int vehicleId) const;

    // Стоит ли у игрока указатель (машина ИЛИ точка). Bounds-safe (false для
    // невалидного id).
    bool hasWaypoint(int playerId) const;

  private:
    CheckpointService *m_checkpoints = nullptr; // владелец клиентского чекпоинт-слота (bind)

    // Per-player целевой vehicleId указателя: -1 — указателя нет ЛИБО цель —
    // статическая точка (showPoint). Различать «нет цели» и «точка» этому полю не
    // нужно: findByVehicle(-1) всегда возвращает -1 (см. её контракт), а
    // hasWaypoint проверяется отдельным флагом m_hasTarget.
    struct Target
    {
        int vehicleId = -1;
        bool hasTarget = false; // указатель стоит (машина или точка)
        bool gps = false;       // цель поставлена /gps (для hasGpsWaypoint/clearGpsFor)
    };
    std::array<Target, MAX_PLAYERS> m_targets{};
};
