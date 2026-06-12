#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "player.hpp"
#include "types.hpp"
#include <Server/Components/Vehicles/vehicles.hpp>
#include <array>
#include <string>

// Сервис машин — источник правды о том, кто в какой машине сидит, и о серверном
// HP каждой машины, плюс валидации.
//
// HP машины клиент-авторитетно: его диктует driver sync водителя. Поэтому сервис
// ведёт принятое HP по модели игрокового здоровья: снижение (урон) принимается,
// рост без серверной санкции — vehicle repair hack — откатывается и фиксируется.
//
// Unoccupied sync (физику пустой машины считает ближайший клиент) — канал чита
// для перетаскивания/телепорта чужих машин: валидируется дистанция репортера,
// величина скачка и скорость; фейковый апдейт ОТКЛОНЯЕТСЯ (не применяется ядром).
//
// Телепорт машины С ВОДИТЕЛЕМ ловится валидатором позиции игрока (позиция
// водителя следует за машиной) — здесь не дублируется.
//
// МАШИНЫ НЕ ВЗРЫВАЮТСЯ ВООБЩЕ, ни по какой причине: ниже ~250 HP клиент
// поджигает машину и затем взрывает. Как только серверное HP падает к порогу,
// машина ГЛОХНЕТ: HP клампится на STALL_HEALTH (выше порога пожара —
// восстановление HP тушит уже занявшийся клиентский огонь), двигатель
// глушится и не заводится до repair(). Единственное, что сервер физически не
// перехватывает, — мгновенный локальный подрыв взрывчаткой вплотную (клиент
// успевает отыграть смерть до ответа сервера).
//
// КОНТРАКТ: серверные изменения HP/ремонт — только через сервис; прямой
// vehicle.setHealth() мимо него валидатор посчитает читерским ростом.
class VehicleService final : public IService
{
  public:
    // Порог «заглохла»: с запасом выше клиентского порога пожара (250).
    static constexpr float STALL_HEALTH = 300.0f;

    void bind(IVehiclesComponent *vehicles, PlayerLocationService &location);

    // --- источник правды ---
    IVehicle *getVehicle(int playerId) const; // машина игрока (по принятому стейту)
    int getSeat(int playerId) const;          // -1 — не в машине; 0 — водитель
    int getDriver(int vehicleId) const;       // id водителя или -1
    float getHealth(int vehicleId) const;     // серверное HP машины
    bool isStalled(int vehicleId) const;      // заглохла (HP добит до порога)

    // --- серверные операции ---
    void setHealth(IVehicle &vehicle, float health);
    void repair(IVehicle &vehicle); // полный ремонт: HP 1000 + визуал + снимает «заглохла»
    // Серверно-авторитетный урон машине (стрельба: клиент водителя чужих пуль не
    // видит при lagcomp — урон применяет сервер; модель ровно как HP игрока).
    void applyDamage(IVehicle &vehicle, float amount);
    void setEngine(IVehicle &vehicle, bool on); // заглохшую завести нельзя (сначала repair)
    void setLocked(IVehicle &vehicle, bool locked);

    // Байпас валидации unoccupied-синка для машины, которую легально двигает
    // сервер (редактор мира): телепорты машины — серверная правда, а
    // редактирующий игрок (репортер синка) может быть телом далеко от камеры.
    // Включается редактором на время жизни сущности, сбрасывается при
    // уничтожении машины.
    void setEditBypass(int vehicleId, bool enable);

    // Серверный тюнинг (бизнес-логика тюнинг-салонов, /nitro и т.п.).
    // Клиентские заявки на моды валидируются в validateMod: вне мод-шопа — чит.
    void addComponent(IVehicle &vehicle, int component);
    void removeComponent(IVehicle &vehicle, int component);
    void setPaintJob(IVehicle &vehicle, int paintjob);

    struct Outcome
    {
        bool vehicleHack = false;
        std::string detail;
    };

    // --- вызывается VehicleSystem ---
    void bindOccupant(IPlayer &player, PlayerState newState); // на смене стейта
    Outcome verifyHealth(IPlayer &player, TimePoint now);     // на апдейте (водитель)
    // vehicleHack в Outcome означает «апдейт отклонить» (система вернёт false ядру).
    Outcome validateUnoccupied(IVehicle &vehicle, IPlayer &reporter, const UnoccupiedVehicleUpdate &update,
                               TimePoint now);
    Outcome validateTrailer(IPlayer &reporter, IVehicle &trailer, TimePoint now);

    // Клиентская заявка на мод: легальна только от водителя ЭТОЙ машины внутри
    // мод-шопа и с валидным id компонента. Иначе — отклонить + нарушение.
    Outcome validateMod(IPlayer &player, IVehicle &vehicle, int component, TimePoint now);
    // Перекраска (Pay'n'Spray / мод-шоп): легальна от водителя; принятие даёт
    // санкцию ремонта — Pay'n'Spray чинит машину на клиенте.
    Outcome validateRespray(IPlayer &player, IVehicle &vehicle, TimePoint now);
    // Вход/выход мод-шопа: покупка чинит машину — санкция ремонта.
    void onModShop(IPlayer &player, TimePoint now);
    void onVehicleCreated(IVehicle &vehicle);
    void onVehicleDestroyed(IVehicle &vehicle);
    void onVehicleRespawn(IVehicle &vehicle); // респаун — HP снова полное
    void onVehicleDeath(IVehicle &vehicle);
    void resetPlayer(int playerId);

  private:
    struct VehicleState
    {
        bool exists = false;
        float health = 1000.0f; // серверное HP
        int driverId = -1;      // обратный индекс «машина -> водитель»
        bool editBypass = false; // машину двигает сервер (редактор) — синк не валидируем
        bool stalled = false;   // заглохла: HP на клампе, двигатель не заводится
        TimePoint lastChange;   // грейс после серверного изменения
        TimePoint lastFlag;     // rate limit нарушений
    };

    struct Occupant
    {
        int vehicleId = -1;
        int seat = -1;
    };

    // Санкция «машину могли починить легально» (мод-шоп, Pay'n'Spray).
    void sanctionRepair(int vehicleId, TimePoint now);
    bool isDriverOf(int playerId, const IVehicle &vehicle) const;
    // HP у порога — кламп над пожаром + глушим (вызывать после падения HP).
    void stallIfCritical(IVehicle &vehicle, VehicleState &st, TimePoint timeNow);
    // Снять «заглохла» (ремонт/респаун): вернуть двигателю клиентский авто-режим.
    void clearStall(IVehicle &vehicle, VehicleState &st);

    IVehiclesComponent *m_vehicles = nullptr;
    PlayerLocationService *m_location = nullptr;

    std::array<VehicleState, VEHICLE_POOL_SIZE> m_vehicleState;
    std::array<Occupant, MAX_PLAYERS> m_occupants;
};
