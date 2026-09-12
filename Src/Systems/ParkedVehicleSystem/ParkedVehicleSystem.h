#pragma once

#include "Macro.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <unordered_map>

// Привод бизнес-фичи «Припаркованные у дома машины» (НЕ Core):
//  * в конструкторе связывает ParkedVehicleService с VehicleService/FamilyService
//    (bind) и подписывается:
//     - subscribeDriverGate: чужой НЕ садится за руль припаркованной машины —
//       личную водит только владелец, расшаренную только члены семьи (серверный
//       гейт по записи Parked; текст отказа различает личную/семейную);
//     - subscribeDestroyed: экземпляр уничтожен (unpark/park/внешний destroy) ->
//       обнулить vehicleId живого экземпляра + снять висящий снимок fuel (см.
//       m_pendingFuelRestore). НЕ путать с death: died НЕ уничтожает Parked-машину
//       (PersonalVehicleSystem трогает только Owner::Player), ядро переспавнит
//       ТОТ ЖЕ id -> destroyed не приходит, vehicleId сохраняется;
//     - subscribeUnsanctionedDeath: владелец МАШИНЫ (не члены семьи) online получает
//       строку про исход детонации — цела на месте либо эвакуирована к дому (резолв
//       по записи Parked: vehicleId -> ownerAccountId -> playerByAccount). Заодно
//       снимает остаток топлива в m_pendingFuelRestore ДО возврата машины;
//     - subscribeRespawned: восстанавливает персистентный остаток поверх дефолтного
//       полного бака, которым VehicleService уже наполнил машину (закрывает
//       «бесплатный эвакуатор с заправкой», см. Docs/GameDesign/Economy.md) —
//       приоритетно из m_pendingFuelRestore (несанкционированная смерть), иначе из
//       самой записи Parked::fuel (явный «Респавн» из /car —
//       ParkedVehicleService::respawnHome снимает fuel в запись ДО respawn());
//  * initialize: через FamilyService::subscribeLoaded (строго после загрузки семей —
//    они нужны для гейта РАСШАРЕННЫХ; личные семей не требуют) грузит parked_vehicle
//    (включая персистентный fuel). На старте спавнит ТОЛЬКО расшаренные семье
//    (family_id != NO_FAMILY) — они живут в мире независимо от онлайна владельца.
//    Личные (NO_FAMILY) на старте НЕ спавнятся (владельцы оффлайн) — их экземпляр
//    заведётся на входе владельца. Расшаренную с несуществующей семьёй НЕ теряет —
//    деградирует в личную (машина ценна сама по себе).
//  * подписки PlayerSessionService start/end: вход владельца -> его ЛИЧНЫЕ припаркованные
//    появляются в мире на своих точках (с персистентным fuel); выход -> исчезают,
//    СНАЧАЛА сняв их fuel (запись+БД целы, респавн на следующем входе с тем же
//    остатком). Расшаренные семье НЕ трогаются. subscribeReconcile сервиса:
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

    // Несанкционированная смерть (контракт смерти VehicleService): только владелец
    // МАШИНЫ (не члены семьи — им ничего не меняется, машина стоит на своей точке)
    // online получает строку про эвакуацию к дому. returnsInPlace различает
    // «на месте»/«эвакуирована». Резолв по записи Parked (vehicleId -> dbId ->
    // ownerAccountId), не по тегу VehicleService (ownerId тега служебный/-1). Снимает
    // остаток топлива в m_pendingFuelRestore ДО возврата (машина ещё валидна).
    void onUnsanctionedDeath(IVehicle &vehicle, bool returnsInPlace);

    // Машина переспавнилась (VehicleService уже дал ПОЛНЫЙ бак по умолчанию). Не-Parked
    // (не наша запись) — no-op, полный бак не трогаем. Иначе восстанавливает остаток:
    // приоритетно из m_pendingFuelRestore (снимок несанкционированной смерти,
    // onUnsanctionedDeath — точнее, взят СРАЗУ после детонации), иначе из самой записи
    // Parked::fuel (явный «Респавн» из /car — ParkedVehicleService::respawnHome снимает
    // fuel в запись ДО respawn(); либо ядровой death-таймер на повторной смерти) —
    // ОДИН путь восстановления на все три источника вызова.
    void onVehicleRespawned(IVehicle &vehicle);

    // Загрузка parked_vehicle из БД (в success-колбэке subscribeLoaded): регистрация
    // Parked (модель/точка/fuel) + спавн ТОЛЬКО расшаренных семье (личные ждут входа
    // владельца). Расшаренная без семьи -> деградация в личную (машина не теряется).
    void loadParked();

    // Онлайн ли аккаунт-владелец: резолв через PlayerSessionService (единственный
    // источник правды об онлайне). NO_ACCOUNT — не онлайн. O(1).
    bool accountOnline(ParkedVehicleService::AccountId accountId) const;

    // Завести живой экземпляр записи, если его ещё нет (vehicleId != -1 -> no-op).
    // Служебный ownerId тега -1 (доступ — canDrive по записи). Применяет персистентный
    // fuel записи поверх дефолтного полного бака от create. Идемпотентно.
    void spawnInstance(long long dbId);
    // Уничтожить живой экземпляр, СОХРАНИВ запись и БД-строку (это НЕ unpark).
    // СНАЧАЛА снимает остаток топлива (память + write-through UPDATE), ПОТОМ
    // vehicleId -> -1. Порядок: setVehicleId(-1) ДО destroy (destroyed-наблюдатель
    // no-op). Идемпотентно (нет экземпляра -> no-op).
    void despawnInstance(long long dbId);
    // Единая точка приведения: desired = машина вызвана И вызвавший в игре; спавн
    // при desired && vehicleId==-1, деспавн при !desired && vehicleId!=-1.
    void reconcile(long long dbId);

    // Write-through UPDATE parked_vehicle.fuel по dbId. Ошибка лишь логируется —
    // память уже верна, зеркало из БД подтвердит на следующем старте.
    void persistFuel(long long dbId, float fuel);

    // Вход/выход владельца: спавн/деспавн его ЛИЧНЫХ (NO_FAMILY) записей. Расшаренные
    // семье НЕ трогаются (живут независимо от онлайна владельца).
    void onCallerOffline(ParkedVehicleService::AccountId accountId);

    ParkedVehicleService &m_parkedService;
    VehicleService &m_vehicleService;
    FamilyService &m_familyService;
    PlayerSessionService &m_sessionService;

    // Снимок остатка топлива на момент НЕсанкционированной смерти (vehicleId ->
    // fuel), ждущий восстановления в subscribeRespawned. Снимается в
    // onVehicleRespawned (успех) ИЛИ в subscribeDestroyed (машина исчезла НЕ через
    // респавн — vehicleId переиспользуем пулом, без очистки другая машина на этом id
    // ложно унаследовала бы чужой fuel).
    std::unordered_map<int, float> m_pendingFuelRestore;
};
