#pragma once

#include "Macro.h"
#include "Services/Core/GridService/GridService.h"
#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/InventoryService/InventoryService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>

// Канистра с бензином — предмет заправки машины, третий поверх базовой системы вещей
// (после аптечки и инструментов). В конструкторе регистрирует свой тип в
// InventoryService; хранение, персист и дев-выдача (/idev) — на InventorySystem,
// здесь только эффект.
//
// Механика ДОСЛОВНО как у инструментов (Docs/Inventory.md), меняется только результат:
// вместо полной починки одна канистра доливает FUEL_PER_CAN топлива.
//
// Команда /refuel (и «Применить» в /inv): игрок должен стоять У МАШИНЫ и СМОТРЕТЬ на
// неё, дальше играет анимация REFUEL_DURATION, и бак пополняется.
//
// Заправка не мгновенная, поэтому все условия проверяются ДВАЖДЫ — на старте и по
// окончании анимации: за это время игрок мог отойти, сесть в машину, машина могла
// пропасть, а канистра — уйти другим путём. Канистра списывается ТОЛЬКО по факту
// заправки: ушёл — ничего не потерял.
class FuelCanSystem : public BaseSystem
{
  public:
    static constexpr int ITEM_FUELCAN = 3; // тип предмета «Канистра с бензином»
    static constexpr int FUELCAN_MAX = 3;  // максимум в стеке
    // Сколько топлива доливает одна канистра. Кламп по FUEL_CAPACITY — на стороне
    // VehicleService::refuel: перелить сверх бака нельзя.
    static constexpr float FUEL_PER_CAN = 30.0f;

    FuelCanSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    void refuel(IPlayer &player);
    void finishRefuel(IPlayer &player);
    // Машина, у которой игрок стоит И на которую смотрит. nullptr — не нашли/не
    // смотрит; reason заполняется причиной отказа для игрока.
    IVehicle *targetVehicle(int playerId, std::string &reason) const;
    // Кто уже заправляет эту машину (кроме exceptPlayerId), или -1. Одну машину
    // заправляет ОДИН игрок: иначе второй долил бы в уже полный бак и потерял канистру.
    int refuellerOf(int vehicleId, int exceptPlayerId) const;

    InventoryService &m_inventory;
    VehicleService &m_vehicleService;
    GridService &m_gridService;
    PlayerLocationService &m_locationService;
    PlayerStateService &m_stateService;
    PlayerAnimationService &m_animationService;
    PlayerSessionService &m_sessionService;
    TimerService &m_timers;

    // Идёт заправка (индексируется playerId): второй /refuel во время анимации не
    // должен ни запускать вторую, ни списывать вторую канистру.
    std::array<bool, MAX_PLAYERS> m_refuelling{};
    // Машина, которую заправляем: по окончании анимации сверяем, что это всё ещё она.
    std::array<int, MAX_PLAYERS> m_targetVehicle{};
    std::array<TimerService::Handle, MAX_PLAYERS> m_pendingTimer{};
};
