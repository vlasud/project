#pragma once

#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/HouseService/HouseService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <vector>

// Меню личного транспорта (/car) — бизнес-фича (НЕ Core). Дистанционный близнец
// пикапа парковки: тот же источник владения (PersonalVehicleService::owned), тот же
// красный чекпоинт-указатель (через общий VehicleWaypointService), но вызывается
// откуда угодно по карте.
//
// Петля: /car -> LIST-диалог со списком ЛИЧНЫХ машин игрока -> выбор машины ->
// ПОД-ДИАЛОГ действий по ней (LIST): припарковать у дома / убрать с парковки /
// расшарить семье / показать на карте. Событийный (команда/диалог), per-tick нет.
//
// Игрок видит и действует ТОЛЬКО над своими машинами (владение серверное по
// playerId); индекс машины ре-валидируется против актуального owned на каждом шаге;
// указатель раскрывает позицию только СВОЕЙ машины.
//
// Парковка/снятие — ре-тег машины НА МЕСТЕ (тот же vehicleId, без destroy/create):
// игрок остаётся в машине, она стоит где стояла, меняется лишь РЕЖИМ (owner-тег +
// запись parked). Три состояния размещения (ParkedVehicleService — источник правды):
//  * ПРИПАРКОВАТЬ (гараж -> личная у дома): за рулём этой машины, ≤30 м от СВОЕГО
//    дома, машина ещё не припаркована -> setOwner(Parked) + setSpawnPosition(spot) +
//    park (INSERT) + detach из Personal (иначе reset на дисконнекте уничтожил бы её);
//  * УБРАТЬ С ПАРКОВКИ (у дома -> гараж): владелец машины -> setOwner(Player) + attach
//    в Personal + unparkKeepInstance (DELETE, экземпляр НЕ уничтожается);
//  * РАСШАРИТЬ СЕМЬЕ (личная у дома -> семейный доступ): владелец семьи, машина
//    припаркована лично -> shareToFamily (только UPDATE family_id, машина НЕ
//    пересоздаётся). Собственность всегда остаётся у владельца.
// Все предусловия серверные, ре-валидация в обработчике (пункты видны всегда).
class CarMenuSystem : public BaseSystem
{
  public:
    CarMenuSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Пункты под-диалога машины (порядок зависит от состояния — диспетчер по вектору,
    // не по магическим индексам, как FamilySystem).
    enum class Action
    {
        ParkHere,
        Unpark,
        ShareToFamily,
        ShowOnMap,
    };
    // Собрать доступные игроку действия по машине carIndex в порядке показа.
    std::vector<Action> buildActions(int playerId, int carIndex) const;

    // /car: нет машин -> сообщение; иначе LIST-диалог выбора машины.
    void showCarList(IPlayer &player);
    // Под-диалог действий по машине carIndex (валидный на момент показа).
    void showCarActions(IPlayer &player, int carIndex);
    // Действие «Припарковать эту машину здесь» для машины carIndex (предусловия серверные).
    void parkHere(IPlayer &player, int carIndex);
    // Действие «Убрать с парковки» для машины carIndex.
    void unpark(IPlayer &player, int carIndex);
    // Действие «Расшарить семье» для машины carIndex (все предусловия серверные).
    void shareToFamily(IPlayer &player, int carIndex);
    // Действие «Показать на карте» для машины carIndex.
    void showOnMap(IPlayer &player, int carIndex);

    // Live vehicleId владения: у припаркованной — из parked-записи (владение detached,
    // vehicleId=-1); иначе — сессионный из owned. -1, если нигде не в мире. Bounds-safe.
    int liveVehicleId(int ownedVehicleId, long long dbId) const;

    PersonalVehicleService &m_personalService;
    VehicleService &m_vehicleService;
    VehicleWaypointService &m_waypointService;
    PlayerDialogService &m_dialogService;
    FamilyService &m_familyService;
    ParkedVehicleService &m_parkedService;
    HouseService &m_houseService;
    PlayerSessionService &m_sessionService;
};
