#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "player.hpp"
#include "types.hpp"
#include <Server/Components/Vehicles/vehicles.hpp>
#include <array>
#include <functional>
#include <string>
#include <vector>

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
    // Полное HP машины SA (максимум; repair чинит сюда). Единый источник факта
    // «полное HP = 1000» — на него опираются и стейт машины, и индикаторы HUD.
    static constexpr float MAX_HEALTH = 1000.0f;

    // Порог «заглохла»: с запасом выше клиентского порога пожара (250).
    static constexpr float STALL_HEALTH = 300.0f;

    // Бак (баланс — тюнится): полный объём и расход за секунду при заведённом
    // двигателе. 100 / 0.1 ≈ 1000 с ≈ 16 минут езды до пустого.
    static constexpr float FUEL_CAPACITY = 100.0f;
    static constexpr float FUEL_DRAIN_PER_SEC = 0.1f;

    // Владелец машины — НЕпрозрачный серверный тег. Ядро лишь хранит пару
    // (тип, id); политику (кто что спавнит, доступ, персист) решают бизнес-
    // системы поверх. Клиент тег не задаёт.
    enum class Owner
    {
        None,
        Player,
        Faction,
        Work
    };

    // Инициализация: привязывает пул машин и сервисы; регистрирует vehicleEvents и
    // poolEvents в диспатчерах компонента (VehicleSystem передаёт себя как оба).
    void bind(IVehiclesComponent *vehicles, PlayerLocationService &location,
              VehicleEventHandler &vehicleEvents, PoolEventHandler<IVehicle> &poolEvents);

    // Единая точка создания машин (источник правды): создаёт машину в пуле и
    // проставляет владельца. Возвращает nullptr, если пул полон. Стейт машины
    // (exists, HP, полный бак) уже проинициализирован пул-событием создания.
    // Всё создание машин — через этот API; onVehicleCreated остаётся ловушкой
    // для сторонних созданий (owner=None).
    IVehicle *create(int model, Vector3 position, float angle, int colour1, int colour2, Owner owner, int ownerId);

    // Получить машину пула по id (nullptr — нет компонента/несуществующая).
    IVehicle *get(int vehicleId) const;
    // Уничтожить машину пула. No-op без компонента/машины. Пул-событие
    // уничтожения (через VehicleSystem) сбросит стейт и оповестит наблюдателей.
    void destroy(int vehicleId);

    // Наблюдатели жизненного цикла машин — для систем со своим индексом машин.
    // created — после регистрации стейта новой машины;
    // destroyed — пока машина ещё валидна, перед сбросом стейта.
    using VehicleObserver = std::function<void(IVehicle &)>;
    void subscribeCreated(VehicleObserver observer);
    void subscribeDestroyed(VehicleObserver observer);

    // --- источник правды ---
    IVehicle *getVehicle(int playerId) const; // машина игрока (по принятому стейту)
    int getSeat(int playerId) const;          // -1 — не в машине; 0 — водитель
    int getDriver(int vehicleId) const;       // id водителя или -1
    float getHealth(int vehicleId) const;     // серверное HP машины
    bool isStalled(int vehicleId) const;      // заглохла (HP добит до порога)

    // --- владелец (серверный тег, читается бизнес-логикой) ---
    Owner getOwner(int vehicleId) const;   // None для несуществующей/чужой
    int getOwnerId(int vehicleId) const;   // -1 при отсутствии владельца
    void setOwner(IVehicle &vehicle, Owner owner, int ownerId);

    // --- топливо ---
    float getFuel(int vehicleId) const;     // 0 для несуществующей машины
    bool isOutOfFuel(int vehicleId) const;  // пустой бак — двигатель не заводится
    void refuel(IVehicle &vehicle, float amount); // долить (amount>0), кламп на CAP
    void setFuel(IVehicle &vehicle, float amount); // абсолют, кламп 0..CAP
    // Дренаж бака за прошедшие seconds: проход по пулу, расход только у машин с
    // заведённым двигателем (наличие водителя не важно); пустой бак глушит
    // двигатель. Зовётся по таймеру (VehicleSystem), не per-tick.
    void drainFuel(float seconds);

    // --- серверные операции ---
    void setHealth(IVehicle &vehicle, float health);
    void repair(IVehicle &vehicle); // полный ремонт: HP 1000 + визуал + снимает «заглохла»
    // Серверно-авторитетный урон машине (стрельба: клиент водителя чужих пуль не
    // видит при lagcomp — урон применяет сервер; модель ровно как HP игрока).
    void applyDamage(IVehicle &vehicle, float amount);
    void setEngine(IVehicle &vehicle, bool on); // заглохшую завести нельзя (сначала repair)
    void setLights(IVehicle &vehicle, bool on); // фары можно переключать всегда
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
        float health = MAX_HEALTH; // серверное HP
        int driverId = -1;      // обратный индекс «машина -> водитель»
        bool editBypass = false; // машину двигает сервер (редактор) — синк не валидируем
        bool stalled = false;   // заглохла: HP на клампе, двигатель не заводится (снимает repair)
        Owner owner = Owner::None; // серверный тег владельца
        int ownerId = -1;          // id владельца в рамках типа (None — -1)
        float fuel = FUEL_CAPACITY; // топливо в баке
        bool outOfFuel = false;     // пустой бак: двигатель не заводится (снимает refuel)
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

    std::vector<VehicleObserver> m_createdObservers;
    std::vector<VehicleObserver> m_destroyedObservers;

    std::array<VehicleState, VEHICLE_POOL_SIZE> m_vehicleState;
    std::array<Occupant, MAX_PLAYERS> m_occupants;
};
