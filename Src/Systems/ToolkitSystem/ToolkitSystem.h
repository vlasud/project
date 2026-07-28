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

// Инструменты — предмет починки машины, второй поверх базовой системы вещей (первый
// — аптечка). В конструкторе регистрирует свой тип в InventoryService; хранение,
// персист и дев-выдача (/idev) — на InventorySystem, здесь только эффект.
//
// Команда /repair: игрок должен стоять У МАШИНЫ и СМОТРЕТЬ на неё, дальше играет
// анимация REPAIR_DURATION, и машина чинится полностью (VehicleService::repair).
//
// Починка не мгновенная, поэтому все условия проверяются ДВАЖДЫ — на старте и по
// окончании анимации: за пять секунд игрок мог отойти, сесть в машину, машина могла
// пропасть, а инструменты — уйти другим путём. Инструмент списывается ТОЛЬКО по
// факту починки: ушёл — ничего не потерял.
class ToolkitSystem : public BaseSystem
{
  public:
    static constexpr int ITEM_TOOLKIT = 2; // тип предмета «Инструменты» в реестре вещей
    static constexpr int TOOLKIT_MAX = 3;  // максимум в стеке

    ToolkitSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    void repair(IPlayer &player);
    void finishRepair(IPlayer &player);
    // Машина, у которой игрок стоит И на которую смотрит. nullptr — не нашли/не
    // смотрит; reason заполняется причиной отказа для игрока.
    IVehicle *targetVehicle(int playerId, std::string &reason) const;
    // Кто уже чинит эту машину (кроме exceptPlayerId), или -1. Одну машину чинит
    // ОДИН игрок: иначе двое списали бы по инструменту за одну и ту же починку.
    int repairerOf(int vehicleId, int exceptPlayerId) const;

    InventoryService &m_inventory;
    VehicleService &m_vehicleService;
    GridService &m_gridService;
    PlayerLocationService &m_locationService;
    PlayerStateService &m_stateService;
    PlayerAnimationService &m_animationService;
    PlayerSessionService &m_sessionService;
    TimerService &m_timers;

    // Идёт починка (индексируется playerId): второй /repair во время анимации не
    // должен ни запускать вторую, ни списывать второй инструмент.
    std::array<bool, MAX_PLAYERS> m_repairing{};
    // Машина, которую чиним: по окончании анимации сверяем, что это всё ещё она.
    std::array<int, MAX_PLAYERS> m_targetVehicle{};
    std::array<TimerService::Handle, MAX_PLAYERS> m_pendingTimer{};
};
