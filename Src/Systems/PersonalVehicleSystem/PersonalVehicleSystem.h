#pragma once

#include "Macro.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Привод бизнес-фичи «Личный транспорт» (НЕ Core):
//  * привязывает VehicleService к PersonalVehicleService (источник правды о
//    машинах) и подписывается на уничтожение машин — обнуляет id экземпляра во
//    владении, если машину уничтожил кто-то ещё (взрыв/destroy): владение остаётся,
//    машину спавнят заново через парковку (ParkingSystem);
//  * подписывается на СМЕРТЬ машины (subscribeDied): личная машина при реальном
//    уничтожении УДАЛЯЕТСЯ (не висит вреком и не «возвращается»). Урон личную не
//    взрывает — она глохнет как все (анти-грифинг в VehicleService), поэтому смерть
//    редка (мгновенный подрыв вплотную, который сервер не перехватывает);
//  * /pvbuy <id модели> — ДЕБАГ-команда покупки (DEVELOPER_LEVEL, Hidden):
//    регистрирует ВЛАДЕНИЕ (модель), машину НЕ спавнит — спавн через парковку;
//  * жизненный цикл владения — через PlayerSessionService (account-data, по
//    конвенции): на старте сессии грузит модели аккаунта из БД (async-select с
//    serial-guard) в PersonalVehicleService, на конце сессии reset (уничтожить
//    машины + очистить ПАМЯТЬ владения; право владения остаётся в БД).
//
// Лимит и валидация модели — серверная политика внутри PersonalVehicleService;
// здесь только ввод команды, загрузка владения и сообщения игроку.
class PersonalVehicleSystem : public BaseSystem
{
  public:
    PersonalVehicleSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Старт сессии: SELECT моделей владения аккаунта из personal_vehicle ->
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

    // Машина умерла (HP -> 0). Если ЛИЧНАЯ (Owner::Player) — уничтожаем её, чтобы
    // она пропала (не висела вреком). Чужие owner-теги (Faction/Work) не трогаем.
    void onVehicleDied(IVehicle &vehicle);

    PersonalVehicleService &m_personalService;
    VehicleService &m_vehicleService;
    PlayerSessionService &m_sessionService;
};
