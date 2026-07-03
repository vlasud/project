#pragma once

#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/HouseService/HouseService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/VehicleLockService/VehicleLockService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <vector>

// Меню личного транспорта (/car) — бизнес-фича (НЕ Core). Корневое LIST-меню
// («Текущая машина - {имя|не за рулем}» / «Мои машины») — источник владения
// по-прежнему PersonalVehicleService::owned, о машинах — VehicleService, красный
// чекпоинт-указатель — общий VehicleWaypointService, замок — VehicleLockService.
//
// ЕДИНОЕ МЕНЮ МАШИНЫ (showCarMenu) — один и тот же под-диалог для обоих входов:
// тумблеры (Завести-Заглушить / Фары / Двери, динамические лейблы по живому
// экземпляру) + действия (Респавн / Показать на карте / Припарковать здесь /
// Убрать с парковки / Передать-Вернуть от семьи). Все пункты видны ВСЕГДА
// (правило видимости); тумблеры гейтятся «сидит В ЭТОЙ машине» (любое сиденье)
// в обработчике, действия — своими серверными гейтами. Машину идентифицирует
// ЗАХВАЧЕННЫЙ carIndex (индекс owned()) с ре-валидацией на каждом клике; после
// тумблера меню переоткрывается с обновлёнными лейблами.
//
// Входы:
//  * корень «Текущая машина - {имя}» — резолвит carIndex машины, в которой игрок
//    сидит, если она СВОЯ (ownCurrentVehicle: личная сессионная Owner::Player/
//    ownerId==playerId ЛИБО припаркованная ЕГО по accountId сессии; расшаренная
//    чужая — НЕ своя). Не в своей машине — лейбл «не за рулем», клик объясняет;
//  * «Мои машины» (TABLIST_HEADERS «Машина | Где находится», единый словарь
//    placementStatus + оверлей водителя «Ник[id]») -> выбор машины.
//
// «Респавн» убирает вызванную машину на парковку (destroy) либо возвращает
// припаркованную на точку у дома (ParkedVehicleService::respawnHome, БЕЗ
// бесплатного топлива — снимок/восстановление бака уже в ParkedVehicleSystem).
//
// Навигация: «Назад» единого меню ведёт туда, откуда пришли (MenuOrigin: корень
// либо «Мои машины»); «Мои машины» -> корень. Правая кнопка корня — «Закрыть»
// (родителя нет). Событийный (команда/диалог), per-tick нет.
class CarMenuSystem : public BaseSystem
{
  public:
    CarMenuSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Пункты единого меню машины (порядок зависит от состояния — диспетчер по
    // вектору, не по магическим индексам, как FamilySystem).
    enum class Action
    {
        Engine, // тумблеры: лейбл динамический по живому экземпляру
        Lights,
        Lock,
        Respawn,
        ShowOnMap,
        ParkHere,
        Unpark,
        ShareToFamily, // лейбл динамический: «Передать семье» / «Вернуть от семьи»
    };

    // Откуда открыто единое меню машины — туда же ведёт «Назад» и переоткрытие
    // после тумблера.
    enum class MenuOrigin
    {
        Root,   // корень /car («Текущая машина»)
        MyCars, // список «Мои машины»
    };

    // --- корень /car ---
    void showRoot(IPlayer &player);

    // --- вход «Текущая машина» ---
    // Живая машина, в которой playerId сидит СЕЙЧАС, если она СВОЯ (личная его
    // сессионная либо припаркованная ИМ) — иначе nullptr. Клиенту не доверяем:
    // сверка по серверным записям.
    IVehicle *ownCurrentVehicle(int playerId) const;
    // Индекс owned() машины, в которой игрок сидит (через ownCurrentVehicle);
    // -1 — не в своей машине либо владение ещё не зеркалировано.
    int currentCarIndex(int playerId) const;
    void showCurrentVehicle(IPlayer &player);

    // --- единое меню машины (тумблеры + действия) ---
    // Собрать пункты меню по машине carIndex в порядке показа.
    std::vector<Action> buildActions(int playerId, int carIndex) const;
    // Список личных машин («Мои машины»); пустой -> сообщение, в диалог не заходим.
    void showMyCars(IPlayer &player);
    // Единое меню машины carIndex; origin — куда ведёт «Назад»/переоткрытие.
    void showCarMenu(IPlayer &player, int carIndex, MenuOrigin origin);
    // Живой экземпляр машины записи владения, в котором игрок сидит СЕЙЧАС
    // (любое сиденье); nullptr — не в мире либо игрок не внутри.
    IVehicle *seatedVehicle(int playerId, const PersonalVehicleService::OwnedVehicle &entry) const;
    // Тумблеры: гейт «сидит В ЭТОЙ машине» (любое сиденье) — в обработчике.
    void toggleEngine(IPlayer &player, int carIndex, MenuOrigin origin);
    void toggleLights(IPlayer &player, int carIndex, MenuOrigin origin);
    void toggleLock(IPlayer &player, int carIndex, MenuOrigin origin);
    // «Респавн»: вызванную (сессионную в мире) убирает на парковку (destroy); припаркованную
    // (у дома/в семье) возвращает на точку через ParkedVehicleService::respawnHome.
    void respawnAction(IPlayer &player, int carIndex);
    void parkHere(IPlayer &player, int carIndex);
    void unpark(IPlayer &player, int carIndex);
    void shareToFamily(IPlayer &player, int carIndex);
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
    VehicleLockService &m_lockService;
    ScreenNoticeService &m_screenNotice; // попапы отказа двигателя (общие с клавишей)
};
