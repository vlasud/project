#pragma once

#include "Macro.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Привод бизнес-фичи «Припаркованные у дома машины» (НЕ Core):
//  * в конструкторе связывает ParkedVehicleService с VehicleService/FamilyService
//    (bind) и подписывается:
//     - subscribeDriverGate: чужой НЕ садится за руль припаркованной машины —
//       личную водит только владелец, расшаренную только члены семьи (серверный
//       гейт по записи Parked; текст отказа различает личную/семейную);
//     - subscribeDestroyed: экземпляр уничтожен (unpark/park/внешний destroy) ->
//       обнулить vehicleId живого экземпляра. НЕ путать с death: died НЕ уничтожает
//       Parked-машину (PersonalVehicleSystem трогает только Owner::Player), ядро
//       переспавнит ТОТ ЖЕ id -> destroyed не приходит, vehicleId сохраняется;
//  * initialize: через FamilyService::subscribeLoaded (строго после загрузки семей —
//    они нужны для гейта РАСШАРЕННЫХ; личные семей не требуют) грузит parked_vehicle.
//    На старте спавнит ТОЛЬКО расшаренные семье (family_id != NO_FAMILY) — они живут в
//    мире независимо от онлайна владельца. Личные (NO_FAMILY) на старте НЕ спавнятся
//    (владельцы оффлайн) — их экземпляр заведётся на входе владельца. Расшаренную с
//    несуществующей семьёй НЕ теряет — деградирует в личную (машина ценна сама по себе).
//  * подписки PlayerSessionService start/end: вход владельца -> его ЛИЧНЫЕ припаркованные
//    появляются в мире на своих точках; выход -> исчезают (запись+БД целы, респавн на
//    следующем входе). Расшаренные семье НЕ трогаются. subscribeReconcile сервиса:
//    переход режима (share/unshare/крайние случаи) приводит экземпляр к желаемому.
//
// ИНВАРИАНТ: живой экземпляр в мире <=> (family_id != NO_FAMILY) ИЛИ владелец ОНЛАЙН.
// Запись parked_vehicle персистит ВСЕГДА, пока машина припаркована — деспавн экземпляра
// её НЕ удаляет. Спавн/деспавн — по числу записей владельца (мало), O(1) на
// запись; онлайн-резолв O(1); гейт доступа O(1); per-tick работы нет.
class ParkedVehicleSystem : public BaseSystem
{
  public:
    ParkedVehicleSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    // Гейт «за руль»: false — не пускать (высадит VehicleService). Не-Parked машина,
    // владелец личной и член семьи расшаренной проходят. accountId — серверный (из
    // сессии), не от клиента.
    bool onDriverGate(IPlayer &player, IVehicle &vehicle);

    // Загрузка parked_vehicle из БД (в success-колбэке subscribeLoaded): регистрация
    // Parked + спавн ТОЛЬКО расшаренных семье (личные ждут входа владельца). Расшаренная
    // без семьи -> деградация в личную (машина не теряется).
    void loadParked();

    // Онлайн ли аккаунт-владелец: резолв через PlayerSessionService (единственный
    // источник правды об онлайне). NO_ACCOUNT — не онлайн. O(1).
    bool ownerOnline(ParkedVehicleService::AccountId accountId) const;

    // Завести живой экземпляр записи, если его ещё нет (vehicleId != -1 -> no-op).
    // Служебный ownerId тега -1 (доступ — canDrive по записи). Идемпотентно.
    void spawnInstance(long long dbId);
    // Уничтожить живой экземпляр, СОХРАНИВ запись и БД-строку (это НЕ unpark).
    // vehicleId -> -1. Порядок: setVehicleId(-1) ДО destroy (destroyed-наблюдатель
    // no-op). Идемпотентно (нет экземпляра -> no-op).
    void despawnInstance(long long dbId);
    // Единая точка приведения: desired = (family_id != NO_FAMILY) || ownerOnline; спавн
    // при desired && vehicleId==-1, деспавн при !desired && vehicleId!=-1.
    void reconcile(long long dbId);

    // Вход/выход владельца: спавн/деспавн его ЛИЧНЫХ (NO_FAMILY) записей. Расшаренные
    // семье НЕ трогаются (живут независимо от онлайна владельца).
    void onOwnerOnline(ParkedVehicleService::AccountId accountId);
    void onOwnerOffline(ParkedVehicleService::AccountId accountId);

    ParkedVehicleService &m_parkedService;
    VehicleService &m_vehicleService;
    FamilyService &m_familyService;
    PlayerSessionService &m_sessionService;
};
