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
// («Текущая машина» / «Мои машины») — источник владения по-прежнему
// PersonalVehicleService::owned, о машинах — VehicleService, красный чекпоинт-
// указатель — общий VehicleWaypointService, замок дверей — VehicleLockService.
//
// РАЗДЕЛ «Текущая машина»: гейт при выборе — игрок сидит В СВОЕЙ машине (любое
// сиденье): личная сессионная (Owner::Player, ownerId == playerId) ЛИБО
// припаркованная ЕГО машина (владелец записи ParkedVehicleService по accountId
// сессии). Расшаренная семье машина ЧУЖОГО владельца — НЕ своя (тумблеры/замок
// доступны только владельцу). Под-диалог тумблеров (двигатель/фары/замок) с
// динамическими лейблами; каждый клик ре-валидирует «свою машину» заново
// (диалог мог висеть, пока игрок вышел из машины/она исчезла) и переоткрывает
// то же под-меню с обновлёнными лейблами.
//
// РАЗДЕЛ «Мои машины» — прежний корень (TABLIST_HEADERS: «Машина | Где
// находится», статус — единый словарь placementStatus) -> под-диалог действий по
// машине (LIST): Респавн / Показать на карте / Припарковать здесь / Убрать с
// парковки / Передать-Вернуть от семьи. Пункты видны ВСЕГДА (правило видимости);
// «Респавн» убирает вызванную машину в гараж (destroy) либо возвращает
// припаркованную на точку у дома (VehicleService::respawn через
// ParkedVehicleService::respawnHome, БЕЗ бесплатного топлива — снимок/восстановление
// бака уже в ParkedVehicleSystem). Машину идентифицирует ЗАХВАЧЕННЫЙ carIndex с
// ре-валидацией по актуальному owned() на каждом клике.
//
// Навигация: под-диалог действий -> «Мои машины» -> корень; «Текущая машина» ->
// корень. Правая кнопка корня — «Закрыть» (родителя нет). Событийный
// (команда/диалог), per-tick нет.
class CarMenuSystem : public BaseSystem
{
  public:
    CarMenuSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Пункты под-диалога действий «Мои машины» (порядок зависит от состояния —
    // диспетчер по вектору, не по магическим индексам, как FamilySystem).
    enum class Action
    {
        Respawn,
        ShowOnMap,
        ParkHere,
        Unpark,
        ShareToFamily, // лейбл динамический: «Передать семье» / «Вернуть от семьи»
    };

    // --- корень /car ---
    void showRoot(IPlayer &player);

    // --- раздел «Текущая машина» ---
    // Живая машина, в которой playerId сидит СЕЙЧАС, если она СВОЯ (личная его
    // сессионная либо припаркованная ИМ) — иначе nullptr (гейт для показа/ре-валид.
    // под-меню тумблеров). Клиенту не доверяем: сверка по серверным записям.
    IVehicle *ownCurrentVehicle(int playerId) const;
    void showCurrentVehicle(IPlayer &player);
    void toggleEngine(IPlayer &player);
    void toggleLights(IPlayer &player);
    void toggleLock(IPlayer &player);

    // --- раздел «Мои машины» ---
    // Собрать доступные игроку действия по машине carIndex в порядке показа.
    std::vector<Action> buildActions(int playerId, int carIndex) const;
    // Список личных машин («Мои машины»); пустой -> сообщение, в диалог не заходим.
    void showMyCars(IPlayer &player);
    // Под-диалог действий по машине carIndex (валидный на момент показа).
    void showCarActions(IPlayer &player, int carIndex);
    // «Респавн»: вызванную (сессионную в мире) убирает в гараж (destroy); припаркованную
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
