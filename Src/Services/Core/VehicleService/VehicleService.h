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

class GridService; // пространственный индекс машин — для anyVehicleNear (указатель-член)

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
    //
    // Parked — припаркованная у дома машина (личная owner-only ИЛИ расшаренная
    // семье). Тег лишь маркирует, что PersonalVehicleSystem её при смерти НЕ
    // удаляет (ранний return не по Owner::Player) — ядро переспавнит её на
    // spawn-позиции = точке у дома. Доступ (кто за руль) решает driver-gate по
    // записи ParkedVehicleService (по vehicleId), НЕ по ownerId тега.
    enum class Owner
    {
        None,
        Player,
        Faction,
        Work,
        Parked
    };

    // Инициализация: привязывает пул машин и сервисы; регистрирует vehicleEvents и
    // poolEvents в диспатчерах компонента (VehicleSystem передаёт себя как оба).
    // grid — пространственный индекс (anyVehicleNear спрашивает соседей машин у него,
    // а не проходом по пулу). Все сервисы сконструированы до фазы initialize, так что
    // grid уже валиден, даже если GridService в реестре идёт после VehicleService.
    void bind(IVehiclesComponent *vehicles, PlayerLocationService &location, GridService &grid,
              VehicleEventHandler &vehicleEvents, PoolEventHandler<IVehicle> &poolEvents);

    // Единая точка создания машин (источник правды): создаёт машину в пуле и
    // проставляет владельца. Возвращает nullptr, если пул полон. Стейт машины
    // (exists, HP, полный бак) уже проинициализирован пул-событием создания.
    // Всё создание машин — через этот API; onVehicleCreated остаётся ловушкой
    // для сторонних созданий (owner=None).
    IVehicle *create(int model, Vector3 position, float angle, int colour1, int colour2, Owner owner, int ownerId);

    // Получить машину пула по id (nullptr — нет компонента/несуществующая).
    IVehicle *get(int vehicleId) const;
    // Есть ли существующая машина в радиусе от точки (кроме excludeVehicleId).
    // Близость ГОРИЗОНТАЛЬНАЯ — по XY, Z игнорируется (точки занятости на одной
    // высоте, машина оседает на грунт со своей Z; иначе Z-разница ложно вышибала бы
    // машину из радиуса). Сравнение по квадрату дистанции (без sqrt). Соседей даёт
    // GridService (пространственный индекс) — O(машин в соседних ячейках), не проход
    // по всему пулу. Нет грида/компонента -> false.
    bool anyVehicleNear(Vector3 position, float radius, int excludeVehicleId = -1) const;
    // Уничтожить машину пула. No-op без компонента/машины. Пул-событие
    // уничтожения (через VehicleSystem) сбросит стейт и оповестит наблюдателей.
    void destroy(int vehicleId);

    // Наблюдатели жизненного цикла машин — для систем со своим индексом машин.
    // created — после регистрации стейта новой машины;
    // destroyed — пока машина ещё валидна, перед сбросом стейта;
    // died — на смерть машины (HP -> 0), машина ещё валидна. Бизнес может
    // реагировать (напр., убрать личную машину, чтобы она не висела вреком).
    // Это ОБЩАЯ инфраструктура: Core лишь оповещает о событии смерти, без
    // бизнес-политики.
    using VehicleObserver = std::function<void(IVehicle &)>;
    void subscribeCreated(VehicleObserver observer);
    void subscribeDestroyed(VehicleObserver observer);
    void subscribeDied(VehicleObserver observer);

    // Вето на посадку ЗА РУЛЬ — общая инфраструктура доступа к машине (Core лишь
    // предоставляет крючок; политику — членство/оплата/бан — решает бизнес). Зовётся
    // из bindOccupant в момент, когда игрок стал водителем (seat==0). Наблюдатель
    // возвращает false, чтобы ОТКАЗАТЬ — сервис высадит игрока (removeFromVehicle,
    // force: отменяет и уже занятое место, и незавершённый вход). Пассажиров НЕ
    // гейтит — зовётся только на driver-ветке. Наблюдатель не должен трогать occupancy
    // машины (высадку делает сервис после вето). Главный поток, событийно.
    using DriverGateObserver = std::function<bool(IPlayer &, IVehicle &)>;
    void subscribeDriverGate(DriverGateObserver observer);

    // Наблюдатель смены позиции БЕЗ водителя — для систем с пространственным
    // индексом машин (GridService): под водителем грид ведёт driver-апдейт, а на
    // прочих путях смены реальной позиции (респаун, ПРИНЯТЫЙ unoccupied-синк)
    // машина «телепортируется» помимо него, и индекс становится стейл. Зовётся
    // ТОЛЬКО с серверно-ПРИНЯТОЙ позицией: на респауне — getPosition() (ядро уже
    // на spawn-позиции), на unoccupied — позиция ИЗ принятого апдейта (ядро
    // применит её ПОСЛЕ accept, getPosition() в этот момент ещё старая).
    // Отклонённый читерский апдейт сюда не доходит. Главный поток.
    using VehicleMoveObserver = std::function<void(IVehicle &, Vector3 acceptedPosition)>;
    void subscribeMoved(VehicleMoveObserver observer);

    // --- источник правды ---
    IVehicle *getVehicle(int playerId) const; // машина игрока (по принятому стейту)
    int getSeat(int playerId) const;          // -1 — не в машине; 0 — водитель
    int getDriver(int vehicleId) const;       // id водителя или -1
    float getHealth(int vehicleId) const;     // серверное HP машины
    bool isStalled(int vehicleId) const;      // заглохла (HP добит до порога)
    // Клиентская велосити машины (SA-юниты) — то, что водитель заявил в driver
    // sync. КОСМЕТИКА (спидометр на экране самого водителя), НЕ для логики:
    // значение подделываемо, серверная правда о движении — PlayerVelocityService.
    // {0,0,0} для несуществующей машины.
    Vector3 getVelocity(int vehicleId) const;

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
    // ПРИВИЛЕГИРОВАННАЯ дев-операция: форсирует смерть машины в обход анти-грифинга.
    // Ставит серверное HP в 0 и шлёт клиенту setHealth(0) БЕЗ stallIfCritical, чтобы
    // клиент детонировал машину, отрепортил Health<=0 в driver-sync и сервер
    // диспатчнул onVehicleDeath. Обычный урон по-прежнему глохнет (stallIfCritical /
    // анти-грифинг не трогаем) — это исключение только для теста реального уничтожения.
    void explode(IVehicle &vehicle);
    void repair(IVehicle &vehicle); // полный ремонт: HP 1000 + визуал + снимает «заглохла»
    // Серверно-авторитетный урон машине (стрельба: клиент водителя чужих пуль не
    // видит при lagcomp — урон применяет сервер; модель ровно как HP игрока).
    void applyDamage(IVehicle &vehicle, float amount);
    void setEngine(IVehicle &vehicle, bool on); // заглохшую завести нельзя (сначала repair)
    void setLights(IVehicle &vehicle, bool on); // фары можно переключать всегда
    void setLocked(IVehicle &vehicle, bool locked);
    // Сменить ТОЛЬКО spawn-позицию/угол существующей машины (для death-респавна на
    // её ТЕКУЩУЮ точку), сохранив прочие поля spawnData (модель/цвета/respawnDelay/
    // siren/interior). Через get/setSpawnData — БЕЗ пересоздания машины: ре-тег
    // парковки НА МЕСТЕ должен вернуть машину сюда, если ядро её переспавнит по смерти.
    // Мусорную (NaN/Inf) позицию/угол в spawnData не пускаем. No-op для несуществующей.
    void setSpawnPosition(IVehicle &vehicle, Vector3 position, float angle);

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
    GridService *m_grid = nullptr; // пространственный индекс машин (anyVehicleNear)

    // Оповещает наблюдателей смены позиции (грид) о принятой позиции машины.
    void notifyMoved(IVehicle &vehicle, Vector3 acceptedPosition);

    std::vector<VehicleObserver> m_createdObservers;
    std::vector<VehicleObserver> m_destroyedObservers;
    std::vector<VehicleObserver> m_diedObservers;
    std::vector<VehicleMoveObserver> m_movedObservers;
    std::vector<DriverGateObserver> m_driverGateObservers;

    std::array<VehicleState, VEHICLE_POOL_SIZE> m_vehicleState;
    std::array<Occupant, MAX_PLAYERS> m_occupants;
};
