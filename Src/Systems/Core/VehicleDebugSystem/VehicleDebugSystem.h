#pragma once

#include "Services/Core/GridService/GridService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Vehicles/vehicles.hpp>

// Отладка машин и связанных сервисов:
//  /veh [модель]   — заспавнить машину рядом и сесть (через putInVehicle — санкция);
//  /vput           — сесть в ближайшую машину (поиск через сетку + санкция);
//  /vdel           — удалить машину, в которой сидишь;
//  /vrespawn       — переспавнить машину, в которой сидишь;
//  /vinfo          — серверное HP vs клиентское, место, id, расхождение;
//  /vhp [hp]       — серверное HP через сервис;
//  /vrepair        — серверный ремонт через сервис;
//  /vengine, /vlock — двигатель/замки через сервис;
//  /vhack          — СИМУЛЯЦИЯ repair hack: сырой setHealth(1000) мимо сервиса —
//                    валидатор должен откатить и записать нарушение.
//  /vdev           — меню-диалог: создать машину владельца Work через единый
//                    VehicleService::create + заправить машину, в которой сидишь +
//                    взорвать текущую машину (форсированная смерть в обход стола).
//  /vtune          — меню-диалог тюнинга СВОЕЙ машины (слоты компонентов/цвет/
//                    пейнтджоб) без ручного ввода id; прототип будущего игрового
//                    меню тюнинга поверх того же API VehicleService.
class VehicleDebugSystem : public BaseSystem
{
  public:
    VehicleDebugSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    IVehicle *currentVehicle(IPlayer &player); // машина игрока (любое сиденье) или сообщение об ошибке
    // Машина, ТОЛЬКО если дев за рулём (seat==0) — для мутирующих команд тюнинга:
    // getVehicle отдаёт машину для ЛЮБОГО занятого места, и без гейта дев-пассажир
    // мог бы тюнить ЧУЖУЮ машину. Иначе сообщение + nullptr (как currentVehicle).
    IVehicle *drivenVehicle(IPlayer &player);
    void showDevMenu(IPlayer &player);          // /vdev: тест create+owner+fuel+refuel

    // --- /vtune: меню тюнинга без ручного ввода id ---

    // Корень: по пункту на каждый слот компонентов + «Цвет» + «Пейнтджоб».
    // vehicleId захватывается здесь и ре-валидируется на каждом клике под-меню
    // (дев мог пересесть/машину могли уничтожить, пока диалог висел).
    void showTuneRoot(IPlayer &player);
    // Под-меню компонентов конкретного слота: «Снять» + валидные для МОДЕЛИ детали.
    void showTuneSlot(IPlayer &player, int vehicleId, int slot);
    void showTuneColour(IPlayer &player, int vehicleId);
    void showTunePaintJob(IPlayer &player, int vehicleId);
    // Мастер ручного ввода пары цветов (доп. пункт «Ввести код цвета»): два числовых
    // диалога подряд (цвет1, затем цвет2), без парсинга свободного текста.
    void showTuneColourInput(IPlayer &player, int vehicleId);
    // Машина ещё под ЭТИМ девом за рулём (seat==0, тот же vehicleId) — иначе
    // сообщение + nullptr; общая ре-валидация для всех кликов под-меню тюнинга.
    IVehicle *tuneVehicle(IPlayer &player, int vehicleId);

    VehicleService &m_vehicleService;
    PlayerStateService &m_stateService;
    PlayerLocationService &m_locationService;
    GridService &m_gridService;
    PlayerDialogService &m_dialogService;
};
