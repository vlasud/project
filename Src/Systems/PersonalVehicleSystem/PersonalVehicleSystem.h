#pragma once

#include "Macro.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <unordered_map>

// Привод бизнес-фичи «Личный транспорт» (НЕ Core):
//  * привязывает VehicleService к PersonalVehicleService (источник правды о
//    машинах) и подписывается на уничтожение машин — обнуляет id экземпляра во
//    владении, если машину уничтожил кто-то ещё (взрыв/destroy): владение остаётся,
//    машину спавнят заново через парковку (ParkingSystem);
//  * подписывается на СМЕРТЬ машины (subscribeDied — приходят только серверно-
//    САНКЦИОНИРОВАННЫЕ смерти, т.е. explode()): такая личная машина УДАЛЯЕТСЯ
//    (не висит вреком и не «возвращается»). Несанкционированную смерть
//    (клиентская детонация пустой / фейковый репорт) VehicleService гасит сам:
//    машина возвращается целой на spawn-точку, владение и экземпляр не страдают;
//  * подписывается на subscribeUnsanctionedDeath (только Owner::Player): владелец
//    online получает строку про исход детонации (см. onUnsanctionedDeath), и
//    СНИМАЕТ остаток топлива (getFuel) машины ДО отложенного respawnIfDead —
//    subscribeRespawned восстанавливает его ПОСЛЕ того, как VehicleService дал
//    полный бак по умолчанию (закрывает «бесплатный эвакуатор с заправкой»,
//    см. Docs/GameDesign/Economy.md);
//  * /pvbuy <id модели> — ДЕБАГ-команда покупки (DEVELOPER_LEVEL, Hidden):
//    регистрирует ВЛАДЕНИЕ (модель), машину НЕ спавнит — спавн через парковку;
//  * жизненный цикл владения — через PlayerSessionService (account-data, по
//    конвенции): на старте сессии грузит модели+fuel аккаунта из БД (async-select
//    с serial-guard) в PersonalVehicleService, на конце сессии СНАЧАЛА снимает
//    fuel живых экземпляров (персист UPDATE), ПОТОМ reset (уничтожить машины +
//    очистить ПАМЯТЬ владения; право владения и fuel остаются в БД).
//
// Лимит и валидация модели — серверная политика внутри PersonalVehicleService;
// здесь только ввод команды, загрузка владения и сообщения игроку.
class PersonalVehicleSystem : public BaseSystem
{
  public:
    PersonalVehicleSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Старт сессии: SELECT моделей+fuel владения аккаунта из personal_vehicle ->
    // serial-guard + живой игрок -> PersonalVehicleService::load. Машину НЕ спавним
    // (игрок берёт на парковке).
    void loadOwnership(IPlayer &player, const PlayerSessionService::Session &session);

    // ДЕБАГ-покупка: модель из аргумента, accountId из сессии. Регистрирует владение
    // (без спавна) в памяти, затем персист (persistPurchase).
    void buyDebug(IPlayer &player, int model);

    // Write-through покупки: INSERT строки владения + LAST_INSERT_ID -> setDbId в
    // память (serial-guard + живой игрок). ownedIndex — индекс записи, добавленной
    // buy() (захвачен в момент запуска). Ошибка БД лишь логируется (память уже есть).
    void persistPurchase(int playerId, PlayerSessionService::AccountId accountId, int model, int ownedIndex);

    // Снять остаток топлива ВСЕХ сессионных заспавненных машин игрока (getFuel из
    // VehicleService, пока экземпляры ещё живы) в память (setFuel) + write-through
    // UPDATE personal_vehicle.fuel по dbId. Зовётся ДО PersonalVehicleService::reset
    // (тот уничтожает экземпляры) — иначе снимать было бы уже нечего.
    void captureFuelBeforeReset(int playerId);
    // Write-through UPDATE personal_vehicle.fuel по dbId. dbId < 0 (ещё не присвоен
    // из async-INSERT покупки) — no-op, снимок останется только в памяти до
    // следующего сохранения.
    void persistFuel(long long dbId, float fuel);

    // Машина умерла (серверно-санкционированная смерть, HP -> 0). Если ЛИЧНАЯ
    // (Owner::Player) — снимаем остаток топлива (персист) и уничтожаем её, чтобы
    // она пропала (не висела вреком). Чужие owner-теги (Faction/Work) не трогаем.
    void onVehicleDied(IVehicle &vehicle);

    // Несанкционированная смерть (контракт смерти VehicleService): только
    // ЛИЧНЫЕ (Owner::Player) машины — владелец online получает строку про
    // исход детонации (returnsInPlace различает «цела на месте»/«эвакуирована
    // на парковку»). ownerId тега Player — playerId (сессионный ключ), не accountId.
    // Снимает остаток топлива в m_pendingFuelRestore (машина ещё валидна, возврат
    // ЕЩЁ не произошёл) — subscribeRespawned применит его поверх дефолтного бака.
    void onUnsanctionedDeath(IVehicle &vehicle, bool returnsInPlace);

    // Машина переспавнилась (VehicleService уже дал ПОЛНЫЙ бак по умолчанию):
    // если в m_pendingFuelRestore есть снимок для этого vehicleId (несанкционированная
    // смерть личной машины) — восстановить его через setFuel поверх дефолта, снять
    // снимок. Иначе no-op (обычный респавн/чужой owner-тег полный бак не трогаем).
    void onVehicleRespawned(IVehicle &vehicle);

    PersonalVehicleService &m_personalService;
    VehicleService &m_vehicleService;
    PlayerSessionService &m_sessionService;

    // Снимок остатка топлива на момент НЕсанкционированной смерти (vehicleId ->
    // fuel), ждущий восстановления в subscribeRespawned. Редкое событие — мало
    // записей одновременно; снимается в onVehicleRespawned (успех) ИЛИ в
    // subscribeDestroyed (машина исчезла НЕ через респавн — vehicleId переиспользуем
    // пулом, без очистки другая машина на этом id ложно унаследовала бы чужой fuel).
    std::unordered_map<int, float> m_pendingFuelRestore;
};
