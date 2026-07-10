#include "Services/Core/VehicleService/VehicleService.h"

#include "Services/Core/GridService/GridService.h" // forEachInRadius/gridMask для anyVehicleNear
#include "Services/Core/VehicleService/RepairZones.h" // серверный гейт SCM-санкций ремонта
#include "Services/Core/VehicleService/VehicleModelNames.h"
#include "glm/geometric.hpp"
#include <Server/Components/Vehicles/vehicle_components.hpp> // Impl::isValidComponentForVehicleModel/getVehicleComponentSlot
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fmt/format.h>
#include <span>

namespace
{
// Допуск на дрожание float HP между клиентом и сервером.
constexpr float HEALTH_EPS = 5.0f;


// Грейс после серверного setHealth/repair: клиент применяет RPC.
constexpr std::chrono::milliseconds SYNC_GRACE{1500};

// Rate limit повторных нарушений по одной машине.
constexpr std::chrono::milliseconds FLAG_COOLDOWN{2000};

// Бэкофф несанкционированных смертей машины: повторная смерть в этом окне после
// предыдущей быстрый возврат НЕ взводит — машину вернёт ядровой death-таймер
// (game.vehicle_respawn_time, ~10 с) на её spawn-точку. Это и спасение машины,
// которая после возврата НА МЕСТЕ тут же умирает снова (утопленная), и потолок
// темпа стрим-чёрна при спам-грифинге (наш респавн — максимум раз в окно).
constexpr std::chrono::seconds UNSANCTIONED_DEATH_BACKOFF{60};

// Темп репортов смертей машин от одного игрока: скользящее окно и потолок.
// Порог терпит честный залп (цепная детонация стоянки одним взрывом — до ~10
// машин, кап парковки у дома), но не систематический снос машин.
constexpr std::chrono::seconds DEATH_REPORT_WINDOW{10};
constexpr int DEATH_REPORT_MAX = 10;

// Unoccupied sync легально шлёт клиент, у которого машина застримлена (~300 м);
// дальше — физически невозможно.
constexpr float UNOCCUPIED_REPORTER_MAX = 350.0f;
// Пустая машина за один апдейт не прыгает дальше этого (катится с горки ~ метры).
constexpr float UNOCCUPIED_JUMP_MAX = 15.0f;
// И не движется быстрее (свободное качение под горку).
constexpr float UNOCCUPIED_SPEED_MAX = 60.0f;

// Прицеп должен быть рядом с тягачом репортера.
constexpr float TRAILER_MAX_DIST = 50.0f;

// Минимальное НЕПРЕРЫВНОЕ время в известной ремзоне перед тем, как SCM-событие
// (мод-шоп/Pay'n'Spray) даёт санкцию ремонта. vehicle.getPosition() клиент-
// авторитетна (ядро пишет её безусловно из driver-sync) и суб-пороговый дрейф
// (телепорт медленнее порога, который ловит PlayerLocationService::verify) даёт
// мгновенное появление в зоне без единого флага в журнале. Мгновенная сверка
// «машина в зоне» этот вектор не закрывает — нужен трек истории: машина обязана
// реально подъехать и продержаться в зоне. 3 с с запасом перекрывает время
// физического заезда в гараж/к воротам на любой честной скорости и не мешает
// легитимному тюнингу (визит в шоп занимает секунды-минуты).
constexpr std::chrono::milliseconds ZONE_DWELL_MIN{3000};

// Запас 3D-радиуса ЗАПРОСА к гриду в anyVehicleNear: грид фильтрует по 3D-дистанции,
// а нам нужна XY-близость. Машина оседает на грунт (своя Z), точки занятости заданы
// на фикс. высоте — без запаса 3D-предфильтр мог бы отбросить кандидата, чья XY в
// радиусе, но Z отличается. Финальную проверку всё равно делаем по XY.
constexpr float GRID_VERTICAL_SLACK = 30.0f;

TimePoint now()
{
    return std::chrono::steady_clock::now();
}

bool rateLimited(TimePoint &lastFlag, TimePoint timeNow)
{
    if (timeNow - lastFlag < FLAG_COOLDOWN)
        return true;
    lastFlag = timeNow;
    return false;
}

// Машина у одной из известных ремзон ПРЯМО СЕЙЧАС? Мгновенная геометрическая
// проверка — сама по себе НЕ гейт (vehicle.getPosition() клиент-авторитетна,
// суб-пороговый дрейф позиции достигает зоны без единого флага); используется
// только внутри trackZoneDwell (для входа/выхода из зоны на driver-sync) и как
// «в зоне сейчас» половина составного dwelledInZone. O(зон), без аллокаций;
// NaN-позиция все сравнения проваливает — «не в зоне».
bool nearAnyZone(std::span<const RepairZones::Zone> zones, Vector3 position)
{
    constexpr float radiusSq = RepairZones::ACCEPT_RADIUS * RepairZones::ACCEPT_RADIUS;
    for (const RepairZones::Zone &zone : zones)
    {
        const float dx = position.x - zone.x;
        const float dy = position.y - zone.y;
        const float dz = position.z - zone.z;
        if (dx * dx + dy * dy + dz * dz <= radiusSq)
            return true;
    }
    return false;
}

// Диапазон + валидность компонента ДЛЯ КОНКРЕТНОЙ МОДЕЛИ — единая проверка для
// addComponent/installComponent (не дублируем формулу диапазона в двух местах).
// 1000..1193 — валидные id компонентов SA-MP; isValidComponentForVehicleModel —
// битовая таблица ядра «этот компонент существует у этой модели».
bool isValidComponent(int model, int component)
{
    return component >= 1000 && component <= 1193 && Impl::isValidComponentForVehicleModel(model, component);
}
} // namespace

void VehicleService::bind(IVehiclesComponent *vehicles, PlayerLocationService &location, GridService &grid,
                          VehicleEventHandler &vehicleEvents, PoolEventHandler<IVehicle> &poolEvents)
{
    m_vehicles = vehicles;
    m_location = &location;
    m_grid = &grid;
    if (m_vehicles)
    {
        m_vehicles->getEventDispatcher().addEventHandler(&vehicleEvents);
        m_vehicles->getPoolEventDispatcher().addEventHandler(&poolEvents);
    }
}

IVehicle *VehicleService::get(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicles)
        return nullptr;
    return m_vehicles->get(vehicleId);
}

std::string_view VehicleService::getModelName(int vehicleId) const
{
    IVehicle *vehicle = get(vehicleId);
    if (!vehicle)
        return {};
    // Модель прошла через create (400..611), но сторонние создания не гейтятся —
    // VehicleModelNames::get на невалидной модели сам отвечает пустым string_view.
    return VehicleModelNames::get(vehicle->getModel());
}

bool VehicleService::anyVehicleNear(Vector3 position, float radius, int excludeVehicleId) const
{
    if (!m_grid || !m_vehicles || radius <= 0.0f || !std::isfinite(radius))
        return false;

    const float radiusSq = radius * radius;
    bool found = false;
    // Спрашиваем соседей-машин у пространственного индекса (O(машин в соседних
    // ячейках), без аллокаций), а не проходом по всему пулу. Запрос к гриду — 3D с
    // запасом по Z (GRID_VERTICAL_SLACK): машина оседает на грунт со своей Z, а точки
    // занятости заданы на фикс. высоте — без запаса 3D-предфильтр отбросил бы кандидата,
    // чья XY в радиусе, но Z отличается. ФИНАЛЬНАЯ проверка — ГОРИЗОНТАЛЬНАЯ (XY).
    m_grid->forEachInRadius(
        position, radius + GRID_VERTICAL_SLACK, gridMask(GridEntityType::Vehicle),
        [&](GridEntityType, std::int32_t id, float)
        {
            if (found || id == excludeVehicleId)
                return; // уже нашли / своя машина
            if (id < 0 || id >= VEHICLE_POOL_SIZE || !m_vehicleState[id].exists)
                return; // мусорный id / в пуле машины уже нет (стейт подчистит destroyed)
            IVehicle *vehicle = m_vehicles->get(id);
            if (!vehicle)
                return;
            // Близость считаем ГОРИЗОНТАЛЬНО (XY), Z игнорируем: точки занятости на одной
            // высоте, а машина оседает на грунт (своя Z) — Z-разница раздувала бы дистанцию
            // выше радиуса и пропускала рядом стоящую машину. visit грид не модифицирует.
            const Vector3 delta = vehicle->getPosition() - position;
            if (delta.x * delta.x + delta.y * delta.y <= radiusSq)
                found = true;
        });
    return found;
}

void VehicleService::destroy(int vehicleId)
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicles)
        return;
    m_vehicles->release(vehicleId);
}

void VehicleService::subscribeCreated(VehicleObserver observer)
{
    m_createdObservers.push_back(std::move(observer));
}

void VehicleService::subscribeDestroyed(VehicleObserver observer)
{
    m_destroyedObservers.push_back(std::move(observer));
}

void VehicleService::subscribeDied(VehicleObserver observer)
{
    m_diedObservers.push_back(std::move(observer));
}

void VehicleService::subscribeRespawned(VehicleObserver observer)
{
    m_respawnedObservers.push_back(std::move(observer));
}

void VehicleService::subscribeUnsanctionedDeath(UnsanctionedDeathObserver observer)
{
    m_unsanctionedDeathObservers.push_back(std::move(observer));
}

void VehicleService::subscribeFuelEmpty(FuelEmptyObserver observer)
{
    m_fuelEmptyObservers.push_back(std::move(observer));
}

void VehicleService::subscribeEngineBroken(EngineBrokenObserver observer)
{
    m_engineBrokenObservers.push_back(std::move(observer));
}

void VehicleService::subscribeMoved(VehicleMoveObserver observer)
{
    m_movedObservers.push_back(std::move(observer));
}

void VehicleService::subscribeTuned(TunedObserver observer)
{
    m_tunedObservers.push_back(std::move(observer));
}

void VehicleService::subscribeDriverGate(DriverGateObserver observer)
{
    m_driverGateObservers.push_back(std::move(observer));
}

void VehicleService::subscribePassengerGate(PassengerGateObserver observer)
{
    m_passengerGateObservers.push_back(std::move(observer));
}

void VehicleService::subscribeStreamedInForPlayer(StreamedInForPlayerObserver observer)
{
    m_streamedInForPlayerObservers.push_back(std::move(observer));
}

void VehicleService::notifyMoved(IVehicle &vehicle, Vector3 acceptedPosition)
{
    for (auto &obs : m_movedObservers)
        obs(vehicle, acceptedPosition);
}

void VehicleService::notifyServerTuned(IVehicle &vehicle)
{
    for (auto &obs : m_tunedObservers)
        obs(vehicle);
}

IVehicle *VehicleService::create(int model, Vector3 position, float angle, int colour1, int colour2, Owner owner,
                                 int ownerId)
{
    if (!m_vehicles)
        return nullptr;

    // Валидный диапазон моделей машин SA: create — единая блессед-точка
    // создания, мусорную/невалидную модель в SDK не пропускаем.
    if (model < VehicleModelNames::MIN_MODEL || model > VehicleModelNames::MAX_MODEL)
        return nullptr;

    VehicleSpawnData data;
    data.respawnDelay = Seconds(-1); // отключает респавн ПО ПРОСТОЮ; death-респавн идёт по глобальному конфигу, политику смерти решает бизнес
    data.modelID = model;
    data.position = position;
    data.zRotation = angle;
    data.colour1 = colour1;
    data.colour2 = colour2;
    data.siren = false;
    data.interior = 0;

    IVehicle *vehicle = m_vehicles->create(data);
    if (!vehicle)
        return nullptr; // пул машин полон

    // Создание уже прогнало стейт через onVehicleCreated (exists/HP/полный бак,
    // owner=None). Проставляем владельца поверх готового стейта.
    setOwner(*vehicle, owner, ownerId);
    return vehicle;
}

IVehicle *VehicleService::getVehicle(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS || !m_vehicles)
        return nullptr;
    const int vehicleId = m_occupants[playerId].vehicleId;
    if (vehicleId < 0)
        return nullptr;
    return m_vehicles->get(vehicleId);
}

int VehicleService::getSeat(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return -1;
    return m_occupants[playerId].seat;
}

int VehicleService::getDriver(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return -1;
    return m_vehicleState[vehicleId].driverId;
}

float VehicleService::getHealth(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return 0.0f;
    return m_vehicleState[vehicleId].health;
}

Vector3 VehicleService::getVelocity(int vehicleId) const
{
    // Клиентская велосити (SA-юниты) прямо из пула — кэшированного стейта для неё
    // нет (косметика, в логике не участвует). getVelocity() в SDK не const, но
    // m_vehicles->get отдаёт неконстантный IVehicle*, как и публичный get().
    IVehicle *vehicle = get(vehicleId);
    if (!vehicle)
        return Vector3(0.0f, 0.0f, 0.0f);
    return vehicle->getVelocity();
}

void VehicleService::setHealth(IVehicle &vehicle, float health)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st.health = health < 0.0f ? 0.0f : health;
    st.lastChange = now();
    vehicle.setHealth(st.health);
    stallIfCritical(vehicle, st, now());
}

void VehicleService::explode(IVehicle &vehicle)
{
    const int vehicleId = vehicle.getID();
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE)
        return;
    VehicleState &st = m_vehicleState[vehicleId];
    if (!st.exists)
        return;
    // Дев/привилегированная операция: добиваем HP до нуля В ОБХОД стола. Серверный
    // setHealth(0) сам onVehicleDeath НЕ диспатчит (только шлёт SetVehicleHealth RPC);
    // на нём клиент детонирует машину и репортит смерть (driver-sync Health<=0 или
    // VehicleDeath RPC) -> setDead -> onVehicleDeath. serverKilled — санкция этой
    // смерти: died-наблюдатели сработают, а douse-кламп (douseUnoccupiedFire) не
    // воскресит HP до детонации. НЕ зовём stallIfCritical: иначе HP заклампится на
    // STALL_HEALTH (анти-грифинг) и взрыва не будет. Обычный урон остаётся под
    // столом — это исключение только для дева.
    st.serverKilled = true;
    st.health = 0.0f;
    st.lastChange = now(); // грейс: клиентский репорт 0 verifyHealth примет как снижение
    vehicle.setHealth(0.0f);
}

void VehicleService::repair(IVehicle &vehicle)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st.health = MAX_HEALTH;
    // Ремонт отменяет неотыгранную санкцию смерти (explode до детонации): иначе
    // висящий serverKilled сделал бы позднейшую читерскую смерть «санкционированной».
    st.serverKilled = false;
    st.lastChange = now();
    vehicle.repair(); // полный ремонт: HP + визуальные повреждения
    clearStall(vehicle, st);
}

void VehicleService::applyDamage(IVehicle &vehicle, float amount)
{
    if (amount <= 0.0f)
        return;
    VehicleState &st = m_vehicleState[vehicle.getID()];
    if (!st.exists)
        return;
    st.health -= amount;
    if (st.health < 0.0f)
        st.health = 0.0f;
    st.lastChange = now(); // грейс: честный водитель применит setHealth и сойдётся
    vehicle.setHealth(st.health);
    stallIfCritical(vehicle, st, now());
}

void VehicleService::setEngine(IVehicle &vehicle, bool on)
{
    const VehicleState &st = m_vehicleState[vehicle.getID()];
    if (on && (st.stalled || st.outOfFuel))
        return; // не завести: добитую — repair(), пустую — refuel()
    VehicleParams params = vehicle.getParams();
    params.engine = on ? 1 : 0;
    vehicle.setParams(params);
}

void VehicleService::setLights(IVehicle &vehicle, bool on)
{
    // Фары не зависят от «заглохла»: их можно жечь и на добитой машине.
    VehicleParams params = vehicle.getParams();
    params.lights = on ? 1 : 0;
    vehicle.setParams(params);
}

void VehicleService::stallIfCritical(IVehicle &vehicle, VehicleState &st, TimePoint timeNow)
{
    if (st.health > STALL_HEALTH)
        return;
    // Санкционированная детонация (explode) клампом не «лечится»: эхо driver-sync
    // с Health=0 после setHealth(0) не должно воскресить HP и слать broadcast —
    // смерть должна отыграться (как в douseUnoccupiedFire).
    if (st.serverKilled)
        return;
    // Урон, который сервер видит, машину не взрывает: ниже ~250 клиент
    // поджигает и затем взрывает. Держим HP над порогом пожара (восстановление
    // HP тушит уже занявшийся огонь) и глушим двигатель — ездить на добитой
    // нельзя.
    //
    // Кламп-RPC шлём только когда он что-то меняет: первый переход в stall либо
    // HP реально ниже клампа (belowClamp — в т.ч. клиентский пожар; восстановление
    // HP его тушит). Уже заглохшая машина репортит в driver-sync ровно
    // STALL_HEALTH (~30Гц) — безусловный setHealth был бы broadcast без
    // изменения данных. Повтор belowClamp-отправки темпуется ОТДЕЛЬНЫМ
    // lastClampSend (не чаще SYNC_GRACE): пиннинг HP чуть ниже клампа не
    // превращает кламп в усилитель broadcast ~30Гц × застримленных. lastChange
    // здесь НЕ трогаем: это грейс СЕРВЕРНЫХ операций (repair-детектор) — кламп,
    // освежающий его, держал бы repair-грейс вечно открытым (пиннинг 299 прятал
    // бы repair-hack); эхо STALL_HEALTH после нашего RPC идёт в accept-ветку
    // verifyHealth, рост ему не нужен. Повтор belowClamp — НЕ нарушение: честный
    // водитель ПЕРЕВЁРНУТОЙ машины даёт его каждые SYNC_GRACE минутами
    // (флип-урон), это штатный сценарий.
    const bool belowClamp = st.health < STALL_HEALTH;
    st.health = STALL_HEALTH;
    if (!st.stalled || (belowClamp && timeNow - st.lastClampSend >= SYNC_GRACE))
    {
        st.lastClampSend = timeNow;
        vehicle.setHealth(STALL_HEALTH);
    }
    if (!st.stalled)
    {
        st.stalled = true;
        VehicleParams params = vehicle.getParams();
        params.engine = 0;
        vehicle.setParams(params);
        // Факт поломки — только на переходе и только если за рулём есть водитель
        // (без водителя оповещать некому). Текст решает бизнес-подписчик.
        if (st.driverId >= 0)
        {
            for (auto &obs : m_engineBrokenObservers)
                obs(vehicle.getID(), st.driverId);
        }
    }
}

void VehicleService::trackZoneDwell(VehicleState &st, Vector3 position, TimePoint timeNow)
{
    // Объединённая проверка «в любой известной ремзоне» (мод-шоп ИЛИ
    // Pay'n'Spray) — сама зона (какая именно) перепроверяется отдельно в
    // гейтах; здесь важен только факт непрерывного нахождения У ОДНОЙ ИЗ них,
    // ровно как раньше делал каждый SCM-гейт разрозненно через nearAnyZone.
    const bool inZone = nearAnyZone(RepairZones::MOD_SHOPS, position) || nearAnyZone(RepairZones::PAY_N_SPRAY, position);
    if (inZone && !st.inZoneNow)
    {
        st.zoneEnteredAt = timeNow; // вход — начало отсчёта непрерывности
    }
    else if (!inZone && st.inZoneNow)
    {
        st.zoneEnteredAt = TimePoint{}; // вышел — непрерывность оборвана
    }
    st.inZoneNow = inZone;
}

bool VehicleService::dwelledInZone(const VehicleState &st, std::span<const RepairZones::Zone> zones,
                                   Vector3 position, TimePoint timeNow) const
{
    // «В зоне сейчас» проверяем по КОНКРЕТНОМУ семейству зон, переданному
    // вызывающим (MOD_SHOPS для мод-шопа, PAY_N_SPRAY либо MOD_SHOPS для
    // Pay'n'Spray/respray) — trackZoneDwell трекует объединённо, а тут нужна
    // точная зона события. dwell же (zoneEnteredAt) общий: машина, приехавшая в
    // мод-шоп и там достоявшая ZONE_DWELL_MIN, при развороте прямо к воротам
    // Pay'n'Spray (обе зоны рядом физически не бывают, но контракт по коду —
    // единый: непрерывное пребывание У ЛЮБОЙ ремзоны) не должна ждать заново —
    // машина всё это время была под наблюдением dwell-трека без разрыва.
    if (!nearAnyZone(zones, position))
        return false;
    if (st.zoneEnteredAt == TimePoint{})
        return false; // трек ещё не видел эту машину в зоне (первый апдейт/дрейф без driver-sync)
    return timeNow - st.zoneEnteredAt >= ZONE_DWELL_MIN;
}

void VehicleService::clearStall(IVehicle &vehicle, VehicleState &st)
{
    if (!st.stalled)
        return;
    st.stalled = false;
    // Возвращаем двигателю клиентский авто-режим (-1): заводится при посадке, как
    // обычная машина. НО если ещё и пустой бак (outOfFuel) — оставляем выключенным
    // (0): repair чинит HP, не топливо; заведётся только после refuel. Иначе
    // отремонтированная пустая машина ездила бы с пустым баком.
    VehicleParams params = vehicle.getParams();
    params.engine = st.outOfFuel ? 0 : -1;
    vehicle.setParams(params);
}

bool VehicleService::isStalled(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE)
        return false;
    return m_vehicleState[vehicleId].stalled;
}

VehicleService::Owner VehicleService::getOwner(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return Owner::None;
    return m_vehicleState[vehicleId].owner;
}

int VehicleService::getOwnerId(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return -1;
    return m_vehicleState[vehicleId].ownerId;
}

void VehicleService::setOwner(IVehicle &vehicle, Owner owner, int ownerId)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    if (!st.exists)
        return;
    st.owner = owner;
    st.ownerId = owner == Owner::None ? -1 : ownerId;
}

float VehicleService::getFuel(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return 0.0f;
    return m_vehicleState[vehicleId].fuel;
}

bool VehicleService::isOutOfFuel(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE)
        return false;
    return m_vehicleState[vehicleId].outOfFuel;
}

void VehicleService::refuel(IVehicle &vehicle, float amount)
{
    if (!std::isfinite(amount) || amount <= 0.0f)
        return; // NaN/Inf (NaN<=0 == false) не должен отравить fuel
    VehicleState &st = m_vehicleState[vehicle.getID()];
    if (!st.exists)
        return;
    st.fuel += amount;
    if (st.fuel > FUEL_CAPACITY)
        st.fuel = FUEL_CAPACITY;
    // Снимаем «пустой бак»: двигатель снова можно завести — но НЕ заводим сами,
    // игрок заведёт сам через Fire (setEngine теперь разрешён). «Заглохла» (HP) —
    // это другая причина и refuel её не снимает.
    if (st.fuel > 0.0f)
        st.outOfFuel = false;
}

void VehicleService::setFuel(IVehicle &vehicle, float amount)
{
    if (!std::isfinite(amount))
        return; // NaN/Inf не осядет в fuel (NaN сравнения ниже неопределены)
    VehicleState &st = m_vehicleState[vehicle.getID()];
    if (!st.exists)
        return;
    st.fuel = amount < 0.0f ? 0.0f : (amount > FUEL_CAPACITY ? FUEL_CAPACITY : amount);
    if (st.fuel > 0.0f)
    {
        st.outOfFuel = false;
        return;
    }
    // Абсолют в 0 — симметрично secondTick при естественном опустошении: взводим
    // outOfFuel И глушим двигатель, если тот сейчас заведён. Без этого восстановление
    // персистентного fuel=0 (напр. после несанкционированной смерти на сухом баке,
    // см. PersonalVehicleSystem/ParkedVehicleSystem::onVehicleRespawned) оставило бы
    // outOfFuel=false от onVehicleRespawn — игрок завёл бы машину с пустым баком.
    st.outOfFuel = true;
    VehicleParams params = vehicle.getParams();
    if (params.engine != 0)
    {
        params.engine = 0;
        vehicle.setParams(params);
    }
}

void VehicleService::setInfiniteFuel(IVehicle &vehicle, bool enable)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    if (!st.exists)
        return;
    st.infiniteFuel = enable;
    if (enable)
    {
        // Наполняем бак и снимаем «пустой»: сухая машина становится заводимой по
        // обычным правилам (setEngine/Fire), а дренаж secondTick её впредь не
        // трогает и держит полной. Двигатель НЕ заводим сами (как refuel) —
        // «заглохла» по HP это тоже НЕ снимает (иная причина, чинит repair).
        st.fuel = FUEL_CAPACITY;
        st.outOfFuel = false;
    }
    // Выключение — бак/outOfFuel оставляем как есть (был полным — останется полным):
    // обычное поведение возвращается сразу, следующий secondTick снова дренажит.
}

bool VehicleService::hasInfiniteFuel(int vehicleId) const
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE)
        return false;
    return m_vehicleState[vehicleId].infiniteFuel;
}

void VehicleService::secondTick(float seconds)
{
    if (seconds <= 0.0f || !m_vehicles)
        return;
    const float drain = FUEL_DRAIN_PER_SEC * seconds;
    const TimePoint timeNow = now();

    // Один проход по пулу на оба дела (тушение без водителя + дренаж): дешёвые
    // bool-проверки отсекают слоты без работы, get/getParams — только когда
    // работа есть. Зовётся по таймеру, не per-tick.
    for (int vehicleId = 0; vehicleId < VEHICLE_POOL_SIZE; ++vehicleId)
    {
        VehicleState &st = m_vehicleState[vehicleId];
        if (!st.exists)
            continue;

        // Тушение нужно машине БЕЗ водителя (driverId — O(1) обратный индекс из
        // bindOccupant, серверный факт посадки), дренаж — машине с топливом.
        // Бесконечное топливо держит слот «с топливом» при ЛЮБОМ значении бака,
        // иначе гипотетический fuel==0 у флагованной машины пропустил бы пин к
        // полному ниже и инвариант «бак всегда полный» стал бы условным.
        const bool wantDouse = st.driverId < 0 && !st.serverKilled;
        const bool wantFuel = st.fuel > 0.0f || st.infiniteFuel;
        if (!wantDouse && !wantFuel)
            continue;

        IVehicle *vehicle = m_vehicles->get(vehicleId);
        if (!vehicle)
            continue; // машины уже нет в пуле — стейт подчистит destroyed-событие

        if (wantDouse)
            douseUnoccupiedFire(*vehicle, st, timeNow);

        if (!wantFuel)
            continue;

        // Бесконечное топливо: бак не расходуется и держится ПОЛНЫМ (флаг ставит
        // бизнес рабочего транспорта). Пиннинг здесь — источник инварианта «полный
        // бак» на каждом проходе, тушение (douse) выше от флага не зависит.
        if (st.infiniteFuel)
        {
            st.fuel = FUEL_CAPACITY;
            continue;
        }

        // Расход — по факту РАБОТАЮЩЕГО двигателя: 1 жжёт всегда (заведённой её и
        // оставили), -1 (клиентский авто-режим, дефолт создания/clearStall) жжёт
        // только с водителем — без него клиент двигатель не заводил, и нетронутый
        // автопарк (парковки, машины у домов) не опустошается сам.
        VehicleParams params = vehicle->getParams();
        if (params.engine == 0 || (params.engine == -1 && st.driverId < 0))
            continue; // заглушён явно / авто-режим без водителя — не расходуется

        st.fuel -= drain;
        if (st.fuel > 0.0f)
            continue;

        // Бак опустел: глушим двигатель и метим outOfFuel (снимет только refuel).
        // wantFuel (st.fuel > 0.0f) выше отсекает уже-пустые слоты на следующих
        // тиках — переход случается РОВНО ОДИН РАЗ на опустошение, без доп. флага.
        st.fuel = 0.0f;
        st.outOfFuel = true;
        params.engine = 0;
        vehicle->setParams(params);

        // Под водителем — оповестить бизнес фактом (текст решает подписчик; Core
        // не шлёт сообщений). Без водителя (заведена и брошена) уведомлять некому.
        if (st.driverId >= 0)
        {
            for (auto &obs : m_fuelEmptyObservers)
                obs(vehicleId, st.driverId);
        }
    }
}

void VehicleService::douseUnoccupiedFire(IVehicle &vehicle, VehicleState &st, TimePoint timeNow)
{
    // Только машина БЕЗ водителя: с водителем HP ведёт verifyHealth (driver-sync).
    // serverKilled/мёртвую не трогаем: кламп воскресил бы HP до детонации
    // explode() либо дёргал бы мёртвую (deathData ядра), которая ждёт респавна.
    if (st.driverId >= 0 || st.serverKilled || vehicle.isDead())
        return;
    // Ядровое HP без водителя опускает только принятый unoccupied-синк ПАССАЖИРА
    // (ядро пишет health при SeatID != 0, vehicle.cpp updateFromUnoccupied) —
    // фактическое покрытие «пассажир без водителя». У по-настоящему пустой
    // (SeatID == 0) ядровое HP ниже клампа не бывает (серверные пути клампят) —
    // для неё ранний return ниже; её клиентский пожар сервер не видит, детонацию
    // возвращает контракт смерти.
    float coreHp = vehicle.getHealth();
    if (!std::isfinite(coreHp))
        coreHp = 0.0f; // NaN/Inf из синка читера — считаем добитой, кламп перетрёт мусор
    if (coreHp >= STALL_HEALTH)
        return; // выше клампа работы нет, RPC не шлём — стоящая машина не генерит трафик
    st.health = coreHp;
    // Кламп + глушим; broadcast setHealth тушит огонь у симулирующего клиента.
    stallIfCritical(vehicle, st, timeNow);
}

void VehicleService::setLocked(IVehicle &vehicle, bool locked)
{
    VehicleParams params = vehicle.getParams();
    params.doors = locked ? 1 : 0;
    vehicle.setParams(params);
}

void VehicleService::setLockedForPlayer(IVehicle &vehicle, IPlayer &player, bool locked)
{
    // setParamsForPlayer шлёт СВОЙ пакет параметров этому игроку — прочие поля
    // (engine/lights/...) намеренно оставлены в дефолте (-1, «не менять»), у SDK
    // отдельный пер-игроковый набор от общего getParams(). Трогаем только doors.
    VehicleParams params;
    params.doors = locked ? 1 : 0;
    vehicle.setParamsForPlayer(player, params);
}

bool VehicleService::isStreamedInForPlayer(const IVehicle &vehicle, const IPlayer &player) const
{
    return vehicle.isStreamedInForPlayer(player);
}

void VehicleService::setSpawnPosition(IVehicle &vehicle, Vector3 position, float angle)
{
    const int vehicleId = vehicle.getID();
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return;
    // NaN/Inf в spawnData не пускаем (симметрично validateUnoccupied): битая точка
    // осела бы в spawnData и death-респавн ядра выкинул бы машину в никуда.
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        !std::isfinite(angle))
        return;
    // getSpawnData() отдаёт const&, присваивание в локальную — КОПИЯ; оригинал не
    // мутируется до setSpawnData. Меняем только точку/угол — прочие поля сохранены.
    VehicleSpawnData data = vehicle.getSpawnData();
    data.position = position;
    data.zRotation = angle;
    vehicle.setSpawnData(data);
}

void VehicleService::respawn(IVehicle &vehicle)
{
    const int vehicleId = vehicle.getID();
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return;
    vehicle.respawn(); // ядро само телепортирует на spawnData.position и даёт полный бак/HP
}

void VehicleService::addComponent(IVehicle &vehicle, int component)
{
    if (!isValidComponent(vehicle.getModel(), component))
        return; // мусор извне (дев-команда/персист/будущее UI) — no-op, не краш
    vehicle.addComponent(component);
}

void VehicleService::removeComponent(IVehicle &vehicle, int component)
{
    if (component < 1000 || component > 1193)
        return;
    vehicle.removeComponent(component);
}

void VehicleService::installComponent(IVehicle &vehicle, int component)
{
    if (!isValidComponent(vehicle.getModel(), component))
        return;
    const int slot = Impl::getVehicleComponentSlot(component);
    if (slot < 0 || slot >= COMPONENT_SLOT_COUNT)
        return; // VehicleComponent_None/мусорный слот — защитный bounds

    // Один компонент на слот: снимаем текущий (если есть и отличается) ПЕРЕД
    // установкой нового — явный RemoveVehicleComponent-RPC старой детали клиентам
    // перед AddComponent новой, а не расчёт на неявную перезапись mods[slot]
    // внутри ядрового addComponent.
    const int current = vehicle.getComponentInSlot(slot);
    if (current != 0 && current != component)
        removeComponent(vehicle, current);
    addComponent(vehicle, component);
}

void VehicleService::setPaintJob(IVehicle &vehicle, int paintjob)
{
    if (paintjob < 0 || paintjob > 2) // валидный диапазон пейнтджобов SA (0..2 варианта на модель)
        return;
    vehicle.setPaintJob(paintjob);
}

std::pair<int, int> VehicleService::getColour(int vehicleId) const
{
    if (!m_vehicles || vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return {-1, -1};
    IVehicle *vehicle = m_vehicles->get(vehicleId);
    if (!vehicle)
        return {-1, -1};
    const Pair<int, int> colour = vehicle->getColour();
    return {colour.first, colour.second};
}

void VehicleService::setColour(IVehicle &vehicle, int colour1, int colour2)
{
    if (colour1 < 0 || colour1 > 255 || colour2 < 0 || colour2 > 255)
        return; // вне диапазона — ядро молча маскирует & 0xFF, здесь явный no-op
    vehicle.setColour(colour1, colour2);
}

int VehicleService::getPaintJob(int vehicleId) const
{
    if (!m_vehicles || vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return -1;
    IVehicle *vehicle = m_vehicles->get(vehicleId);
    if (!vehicle)
        return -1;
    return vehicle->getPaintJob();
}

void VehicleService::getComponents(int vehicleId, std::vector<int> &out) const
{
    out.clear();
    if (!m_vehicles || vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return;
    IVehicle *vehicle = m_vehicles->get(vehicleId);
    if (!vehicle)
        return;
    for (int slot = 0; slot < COMPONENT_SLOT_COUNT; ++slot)
    {
        const int component = vehicle->getComponentInSlot(slot);
        if (component != 0) // 0 — слот пуст
            out.push_back(component);
    }
}

int VehicleService::getComponentSlot(int component) const
{
    if (component < 1000 || component > 1193)
        return VehicleComponent_None; // диапазон валидных id компонентов SA-MP
    return Impl::getVehicleComponentSlot(component);
}

void VehicleService::componentsForSlot(int model, int slot, std::vector<int> &out) const
{
    out.clear();
    if (slot < 0 || slot >= COMPONENT_SLOT_COUNT)
        return;
    // Полный перебор диапазона (<=194 id) — холодный путь построения меню, не
    // per-tick. isValidComponent (диапазон + isValidComponentForVehicleModel) —
    // та же проверка, что и у addComponent/installComponent, не дублируем формулу.
    for (int component = 1000; component <= 1193; ++component)
    {
        if (Impl::getVehicleComponentSlot(component) == slot && isValidComponent(model, component))
            out.push_back(component);
    }
}

int VehicleService::installedInSlot(int vehicleId, int slot) const
{
    if (!m_vehicles || slot < 0 || slot >= COMPONENT_SLOT_COUNT || vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE ||
        !m_vehicleState[vehicleId].exists)
        return -1;
    IVehicle *vehicle = m_vehicles->get(vehicleId);
    if (!vehicle)
        return -1;
    return vehicle->getComponentInSlot(slot); // 0 — слот пуст
}

void VehicleService::sanctionRepair(int vehicleId, TimePoint timeNow)
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicleState[vehicleId].exists)
        return;
    // Клиент легально починил машину (мод-шоп, Pay'n'Spray) — принимаем полное
    // HP и даём грейс, чтобы repair-hack детектор не дал ложняк. ИНВАРИАНТ: это
    // единственный рост серверного HP по клиентскому событию, поэтому каждый
    // вызов обязан стоять ЗА серверным гейтом локации (машина у ремзоны
    // RepairZones / сессия inModShop) — голому SCM-событию санкцию не выдаём.
    VehicleState &st = m_vehicleState[vehicleId];
    st.health = MAX_HEALTH;
    st.lastChange = timeNow;
    if (IVehicle *vehicle = m_vehicles ? m_vehicles->get(vehicleId) : nullptr)
        clearStall(*vehicle, st); // починенная снова заводится
}

bool VehicleService::isDriverOf(int playerId, const IVehicle &vehicle) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    const Occupant &occupant = m_occupants[playerId];
    return occupant.seat == 0 && occupant.vehicleId == vehicle.getID();
}

void VehicleService::bindOccupant(IPlayer &player, PlayerState newState)
{
    const int playerId = player.getID();
    Occupant &occupant = m_occupants[playerId];

    // Снимаем старую привязку «машина -> водитель», если она была наша.
    if (occupant.seat == 0 && occupant.vehicleId >= 0 && m_vehicleState[occupant.vehicleId].driverId == playerId)
    {
        m_vehicleState[occupant.vehicleId].driverId = -1;
    }

    if (newState == PlayerState_Driver || newState == PlayerState_Passenger)
    {
        // Граница сервиса: queryExtension здесь, потребители ходят через getVehicle().
        IPlayerVehicleData *data = queryExtension<IPlayerVehicleData>(player);
        IVehicle *vehicle = data ? data->getVehicle() : nullptr;
        occupant.vehicleId = vehicle ? vehicle->getID() : -1;
        occupant.seat = data ? data->getSeat() : -1;
        occupant.inModShop = false; // сессия мод-шопа не переживает смену привязки (честной пересадки в шопе нет)

        if (occupant.seat == 0 && occupant.vehicleId >= 0)
        {
            m_vehicleState[occupant.vehicleId].driverId = playerId;

            // Игрок стал водителем — прогоняем вето доступа (членство/оплата и т.п.).
            // Если хоть один наблюдатель отказал, откатываем привязку водителя И
            // occupant ДО высадки: removeFromVehicle(force) через clearTasks может
            // синхронно/на след. тике дать вложенный state-change (Driver->OnFoot),
            // и он обязан увидеть уже чистый OnFoot-слот (иначе повторно снимет чужую
            // привязку). Driver-гейт — только seat==0; пассажиров гейтит ветка ниже.
            if (vehicle && !m_driverGateObservers.empty())
            {
                bool allowed = true;
                for (auto &gate : m_driverGateObservers)
                {
                    if (!gate(player, *vehicle))
                    {
                        allowed = false;
                        break;
                    }
                }
                if (!allowed)
                {
                    m_vehicleState[occupant.vehicleId].driverId = -1;
                    occupant = {};
                    player.removeFromVehicle(true); // отменяет и место, и вход
                }
            }
        }
        else if (occupant.seat > 0 && occupant.vehicleId >= 0)
        {
            // Вето на посадку ПАССАЖИРОМ (seat != 0): бизнес может запретить (напр.
            // рабочий транспорт без пассажиров). Как в driver-ветке, occupant чистим ДО
            // высадки — removeFromVehicle(force) через clearTasks даёт вложенный
            // Passenger->OnFoot стейт-чейндж, и он обязан увидеть уже чистый OnFoot-слот.
            if (vehicle && !m_passengerGateObservers.empty())
            {
                bool allowed = true;
                for (auto &gate : m_passengerGateObservers)
                {
                    if (!gate(player, *vehicle))
                    {
                        allowed = false;
                        break;
                    }
                }
                if (!allowed)
                {
                    occupant = {};
                    player.removeFromVehicle(true);
                }
            }
        }
        return;
    }

    occupant = {};
}

VehicleService::Outcome VehicleService::verifyHealth(IPlayer &player, TimePoint timeNow)
{
    Outcome outcome;

    const Occupant &occupant = m_occupants[player.getID()];

    // Реконсиляция привязки с ядром: driver-sync ядро принимает на ЛЮБУЮ
    // застримленную машину и молча пересаживает водителя (updateFromDriverSync
    // переставляет driver/PlayerVehicleData), а setState(Driver) у уже-Driver
    // стейт-чейндж не диспатчит — bindOccupant не зовётся, occupant остаётся на
    // СТАРОЙ машине: verifyHealth валидировал бы не ту машину, а driver-gate
    // (вето посадки) обходился бы «пересадкой» синком. При расхождении (в норме
    // — сравнение двух int) перепривязываем штатным bindOccupant: оба обратных
    // индекса + driver-gate, вето высадит, как при обычной посадке. Только для
    // seat==0: после вето occupant пуст, а ядро держит Driver, пока клиент не
    // отработал высадку, — реконсиляция пустого слота зациклила бы
    // гейт+removeFromVehicle на каждом апдейте.
    if (occupant.seat == 0)
    {
        IPlayerVehicleData *data = queryExtension<IPlayerVehicleData>(player);
        IVehicle *coreVehicle = data ? data->getVehicle() : nullptr;
        const int coreVehicleId = coreVehicle ? coreVehicle->getID() : -1;
        // Перепривязка нужна и когда наш обратный индекс driverId затёрт ЧУЖИМ
        // форженным driver-sync на ЭТУ же машину: ядро пересаживает водителя без
        // стейт-чейнджа, и m_vehicleState[V].driverId уходит на чужого. Без
        // реклейма onVehicleShot видит V как пустую (getDriver<0) и НЕ применяет
        // серверный урон пулями — god-mode машина сообщника. Сравнение int'ов на
        // чистом пути; сходится к последнему синкающему, как в ядре.
        const bool stolenIndex = coreVehicleId >= 0 && coreVehicleId == occupant.vehicleId &&
                                 m_vehicleState[coreVehicleId].driverId != player.getID();
        if (coreVehicleId != occupant.vehicleId || stolenIndex)
            bindOccupant(player, player.getState()); // occupant/driverId обновятся (тот же слот)
    }

    if (occupant.seat != 0 || occupant.vehicleId < 0) // HP диктует только водитель
        return outcome;

    VehicleState &st = m_vehicleState[occupant.vehicleId];
    if (!st.exists || !m_vehicles)
        return outcome;
    IVehicle *vehicle = m_vehicles->get(occupant.vehicleId);
    if (!vehicle)
        return outcome;

    // Zone dwell-трек — на КАЖДОМ принятом driver-sync (~30Гц под водителем),
    // а не разово в момент SCM-события: только так «непрерывно в зоне не менее
    // ZONE_DWELL_MIN» отражает реальную физику подъезда, а не мгновенную сверку
    // клиент-авторитетной vehicle.getPosition(). NaN/Inf позиция (не должна тут
    // осесть — ядро её не пишет из driver-sync без валидации на своей стороне)
    // nearAnyZone внутри трека провалит как «не в зоне» — трек просто сбросится.
    trackZoneDwell(st, vehicle->getPosition(), timeNow);

    const float reported = vehicle->getHealth(); // заявление клиента водителя

    // Мусорный float (NaN отсеялся бы сравнением ниже, но -inf прошёл бы в
    // принятие и осел в серверном HP) — откат на серверную правду + нарушение:
    // не-конечное/отрицательное HP из honest-клиента невозможно физически.
    // Откат и флаг — в темпе FLAG_COOLDOWN (безусловный откат на каждом апдейте
    // дал бы спамящему NaN читеру усилитель broadcast-трафика ~30Гц × стримящие).
    // lastChange НЕ трогаем: это грейс СЕРВЕРНЫХ операций — клиентский мусор,
    // освежающий его, держал бы repair-грейс вечно открытым и прятал repair-hack.
    if (!std::isfinite(reported) || reported < 0.0f)
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            vehicle->setHealth(st.health);
            outcome.vehicleHack = true;
            outcome.detail =
                fmt::format("vehicle {} bogus health from driver sync: {}", occupant.vehicleId, reported);
        }
        return outcome;
    }

    if (reported <= st.health + HEALTH_EPS)
    {
        // Снижение (урон) или эхо — принимаем. Храповик: принятое HP двигается
        // только ВНИЗ, HEALTH_EPS — допуск «не флажить дрожание» float, а не
        // источник роста (запись reported поверх st.health принимала бы +EPS за
        // синк — тихий подъём серверного HP до MAX мимо repair-детектора,
        // перегоняя applyDamage и снимая stall-кламп). Рост дают только
        // серверные операции — они пишут st.health напрямую (repair/
        // sanctionRepair/setHealth).
        st.health = std::min(st.health, reported);
        stallIfCritical(*vehicle, st, timeNow); // у порога пожара — кламп + глушим
        return outcome;
    }

    if (timeNow - st.lastChange < SYNC_GRACE)
        return outcome; // клиент применяет недавний серверный setHealth/repair

    // Рост HP без серверной санкции — vehicle repair hack. Откатываем.
    vehicle->setHealth(st.health);
    st.lastChange = timeNow;
    if (!rateLimited(st.lastFlag, timeNow))
    {
        outcome.vehicleHack = true;
        outcome.detail =
            fmt::format("vehicle {} repair hack: {:.0f} -> {:.0f}", occupant.vehicleId, st.health, reported);
    }
    return outcome;
}

VehicleService::Outcome VehicleService::validateUnoccupied(IVehicle &vehicle, IPlayer &reporter,
                                                           const UnoccupiedVehicleUpdate &update, TimePoint timeNow)
{
    Outcome outcome;
    VehicleState &st = m_vehicleState[vehicle.getID()];

    // Фабрика detail: строку формируем ТОЛЬКО когда нарушение реально пишется
    // (не rate-limited). Иначе водитель застримленной машины вне зоны, флудя
    // ядро валидными-для-модели SCM/unoccupied-событиями, давал бы heap-аллокацию
    // на каждый отклонённый пакет (ядро эти события не троттлит) — а строка тут же
    // выбрасывалась бы. Теперь формат считается максимум раз в FLAG_COOLDOWN.
    const auto reject = [&](auto &&makeDetail)
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = makeDetail();
        }
        outcome.vehicleHack = true;
        return outcome;
    };

    // Не-конечные позиция/велосити (NaN/Inf от читера): сравнения с NaN ложны, поэтому
    // числовые гейты ниже их пропустят -> ядро применило бы NaN-позицию, машина
    // «спряталась» бы из occupancy (NaN <= radiusSq == false). Режем ДО всех веток
    // (включая editBypass), как прочий клиентский float (ср. refuel/setFuel/verifyHealth).
    if (!std::isfinite(update.position.x) || !std::isfinite(update.position.y) ||
        !std::isfinite(update.position.z) || !std::isfinite(update.velocity.x) ||
        !std::isfinite(update.velocity.y) || !std::isfinite(update.velocity.z))
    {
        return reject([&] { return fmt::format("unoccupied vehicle {} non-finite sync", vehicle.getID()); });
    }

    // Тушение по ходу синков: HP машины без водителя ядро применило из ПРОШЛОГО
    // принятого синка пассажира (в самом апдейте HP нет) — падение к порогу видно
    // уже здесь, окно сжимается до одного синка вместо секундного прохода.
    // Темп-гейт по lastClampSend: кламп чаще раза в SYNC_GRACE не шлём — спам
    // синков с низким HP не превращается в усилитель broadcast'ов (secondTick
    // всё равно дотушит). lastChange не подходит: это грейс серверных операций,
    // кламп его не освежает.
    if (timeNow - st.lastClampSend >= SYNC_GRACE)
        douseUnoccupiedFire(vehicle, st, timeNow);

    // Машину двигает сервер (редактор): её телепорты и дальний репортер легальны.
    if (st.editBypass)
    {
        notifyMoved(vehicle, update.position); // апдейт принят — двигаем грид
        return outcome;
    }

    if (m_location)
    {
        const float reporterDist = glm::distance(m_location->getPosition(reporter.getID()), vehicle.getPosition());
        if (reporterDist > UNOCCUPIED_REPORTER_MAX)
        {
            return reject(
                [&] { return fmt::format("unoccupied sync for vehicle {} from {:.0f}m", vehicle.getID(), reporterDist); });
        }
    }

    const float jump = glm::distance(vehicle.getPosition(), update.position);
    if (jump > UNOCCUPIED_JUMP_MAX)
    {
        return reject([&] { return fmt::format("unoccupied vehicle {} jump {:.0f}m", vehicle.getID(), jump); });
    }

    const float speed = glm::length(update.velocity);
    if (speed > UNOCCUPIED_SPEED_MAX)
    {
        return reject([&] { return fmt::format("unoccupied vehicle {} speed {:.0f} m/s", vehicle.getID(), speed); });
    }

    // Апдейт принят: ядро применит update.position ПОСЛЕ accept (getPosition() здесь
    // ещё старая) — двигаем грид на ПРИНЯТУЮ позицию из апдейта. Отклонённые выше
    // читерские позиции (reject) сюда не доходят и в грид не попадают.
    notifyMoved(vehicle, update.position);
    return outcome; // легально
}

VehicleService::Outcome VehicleService::validateTrailer(IPlayer &reporter, IVehicle &trailer, TimePoint timeNow)
{
    Outcome outcome;
    if (!m_location)
        return outcome;

    VehicleState &st = m_vehicleState[trailer.getID()];

    // Не-конечная позиция прицепа (NaN/Inf от читера) прошла бы dist-гейт мимо (NaN>MAX —
    // ложь) и попала бы в грид. Режем симметрично validateUnoccupied; cellCoord у стока —
    // второй рубеж. (trailerSync ядро применяет безусловно, серверный pos это не откатит —
    // общий NaN-класс; но в грид/occupancy мусор не пускаем и помечаем нарушение.)
    const Vector3 pos = trailer.getPosition();
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = fmt::format("trailer {} non-finite position", trailer.getID());
        }
        outcome.vehicleHack = true;
        return outcome;
    }

    const float dist = glm::distance(m_location->getPosition(reporter.getID()), pos);
    if (dist > TRAILER_MAX_DIST)
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = fmt::format("trailer {} sync from {:.0f}m", trailer.getID(), dist);
        }
        outcome.vehicleHack = true;
        return outcome;
    }

    // Прицеп — IVehicle в гриде, но водителя у него нет (тянет тягач), поэтому driver-путь
    // (onPlayerUpdate seat==0) его не двигает. Единственный путь смены его реальной позиции —
    // trailer-sync; ядро применяет trailerSync-позицию БЕЗУСЛОВНО ДО этого хэндлера, значит
    // getPosition() уже свежая (в отличие от unoccupied, где позиция из update). На принятом
    // (не-читерском) апдейте двигаем грид, иначе запись прицепа замерла бы на старой точке и
    // anyVehicleNear/occupancy видели бы его не там. NaN гасит cellCoord у стока.
    notifyMoved(trailer, trailer.getPosition());
    return outcome;
}

VehicleService::Outcome VehicleService::validateMod(IPlayer &player, IVehicle &vehicle, int component,
                                                    TimePoint timeNow)
{
    Outcome outcome;
    VehicleState &st = m_vehicleState[vehicle.getID()];

    // Фабрика detail: строку формируем ТОЛЬКО когда нарушение реально пишется
    // (не rate-limited). Иначе водитель застримленной машины вне зоны, флудя
    // ядро валидными-для-модели SCM/unoccupied-событиями, давал бы heap-аллокацию
    // на каждый отклонённый пакет (ядро эти события не троттлит) — а строка тут же
    // выбрасывалась бы. Теперь формат считается максимум раз в FLAG_COOLDOWN.
    const auto reject = [&](auto &&makeDetail)
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = makeDetail();
        }
        outcome.vehicleHack = true;
        return outcome;
    };

    if (!isDriverOf(player.getID(), vehicle))
    {
        return reject([&] { return fmt::format("mod {} for vehicle {} not as driver", component, vehicle.getID()); });
    }

    if (component < 1000 || component > 1193) // валидные id компонентов SA-MP
    {
        return reject([&] { return fmt::format("invalid mod component {}", component); });
    }

    // Легальный тюнинг возможен только в сессии мод-шопа, подтверждённой
    // СЕРВЕРОМ (inModShop взводит только зонно-проверенный вход, см. onModShop),
    // и с машиной, НЕПРЕРЫВНО простоявшей в зоне шопа (ворота либо интерьер — во
    // время сессии клиент синкает интерьерную позицию) не меньше ZONE_DWELL_MIN
    // (dwelledInZone, трек — verifyHealth на driver-sync). Голое «в зоне сейчас»
    // (nearAnyZone) доверяло бы клиент-авторитетной vehicle.getPosition() без
    // истории — суб-пороговый дрейф позиции доехал бы досюда без единого флага.
    // Клиентскому isInModShop ядра не верим — его ставит тот же SCM-пакет;
    // повторная сверка зоны не даёт «вечной» сессии с подавленным exit. Чит-меню
    // ставит моды (нитро, гидравлику) где угодно — это и отсекаем.
    const Vector3 position = vehicle.getPosition();
    if (!m_occupants[player.getID()].inModShop || !dwelledInZone(st, RepairZones::MOD_SHOPS, position, timeNow))
    {
        return reject(
            [&] {
                return fmt::format("mod {} outside mod shop at ({:.0f}, {:.0f}, {:.0f})", component, position.x,
                                   position.y, position.z);
            });
    }

    // Заявка ПРОШЛА БЫ старый гейт (честный водитель, честно заехавший в
    // мод-гараж и выбравший деталь) — но клиентский мод-гараж больше НЕ источник
    // тюнинга: отклоняем БЕЗ нарушения (vehicleHack остаётся false), ядро не
    // применит мод (RemoveVehicleComponent RPC откатит визуал этому клиенту —
    // см. vehicles_impl.hpp). sanctionRepair здесь больше не зовём: покупка
    // детали дублировала ремонт с onModShop-enter, который уже чинит машину при
    // въезде — легитимный визит по-прежнему легально чинит машину.
    return outcome;
}

VehicleService::Outcome VehicleService::validatePaintJob(IPlayer &player, IVehicle &vehicle, int paintJob,
                                                          TimePoint timeNow)
{
    Outcome outcome;
    VehicleState &st = m_vehicleState[vehicle.getID()];

    // Фабрика detail: строку формируем ТОЛЬКО когда нарушение реально пишется
    // (не rate-limited). Иначе водитель застримленной машины вне зоны, флудя
    // ядро валидными-для-модели SCM/unoccupied-событиями, давал бы heap-аллокацию
    // на каждый отклонённый пакет (ядро эти события не троттлит) — а строка тут же
    // выбрасывалась бы. Теперь формат считается максимум раз в FLAG_COOLDOWN.
    const auto reject = [&](auto &&makeDetail)
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = makeDetail();
        }
        outcome.vehicleHack = true;
        return outcome;
    };

    if (!isDriverOf(player.getID(), vehicle))
    {
        return reject([&] { return fmt::format("paintjob {} for vehicle {} not as driver", paintJob, vehicle.getID()); });
    }

    if (paintJob < 0 || paintJob > 2) // валидный диапазон пейнтджобов SA (0..2 вариантов на модель)
    {
        return reject([&] { return fmt::format("invalid paintjob {}", paintJob); });
    }

    // Пейнтджоб SA даёт только мод-шоп (Pay'n'Spray его не предлагает) — тот же
    // гейт зоны/сессии/dwell, что и validateMod. Ядро само это событие не
    // проверяет (дефолтная onVehiclePaintJob в SDK возвращает true) — без гейта
    // чит-меню ставило бы косметику где угодно.
    const Vector3 position = vehicle.getPosition();
    if (!m_occupants[player.getID()].inModShop || !dwelledInZone(st, RepairZones::MOD_SHOPS, position, timeNow))
    {
        return reject(
            [&] {
                return fmt::format("paintjob {} outside mod shop at ({:.0f}, {:.0f}, {:.0f})", paintJob, position.x,
                                   position.y, position.z);
            });
    }

    // Легитимный визит — но клиентский пейнтджоб больше не источник тюнинга:
    // отклоняем БЕЗ нарушения. Ядровая onVehiclePaintJob сама НЕ шлёт
    // компенсирующий RPC при false (в отличие от AddComponent) — клиент, уже
    // применивший пейнтджоб визуально до ответа сервера, увидит десинк до
    // следующего полного стрим-ина (см. Docs/Vehicles.md).
    return outcome; // косметика — санкцию ремонта не повторяем (уже дана enter/AddComponent)
}

VehicleService::Outcome VehicleService::validateRespray(IPlayer &player, IVehicle &vehicle, TimePoint timeNow)
{
    Outcome outcome;
    VehicleState &st = m_vehicleState[vehicle.getID()];

    if (!isDriverOf(player.getID(), vehicle))
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = fmt::format("respray of vehicle {} not as driver", vehicle.getID());
        }
        outcome.vehicleHack = true;
        return outcome;
    }

    // Перекраска легальна только у ремзоны, где машина НЕПРЕРЫВНО простояла не
    // меньше ZONE_DWELL_MIN (dwelledInZone — трек verifyHealth на driver-sync,
    // не мгновенная сверка клиент-авторитетной позиции): Pay'n'Spray либо
    // мод-шоп в подтверждённой сессии. Голый SetColour где угодно (в т.ч. после
    // суб-порогового телепорт-дрейфа в зону) — фейковый SCM ради бесплатного
    // серверного ремонта: отклонить БЕЗ санкции; координаты в detail — для
    // ручного разбора (и пополнения RepairZones, если зона реальная).
    const Vector3 position = vehicle.getPosition();
    const bool atPayNSpray = dwelledInZone(st, RepairZones::PAY_N_SPRAY, position, timeNow);
    const bool atModShop =
        m_occupants[player.getID()].inModShop && dwelledInZone(st, RepairZones::MOD_SHOPS, position, timeNow);
    if (!atPayNSpray && !atModShop)
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = fmt::format("respray of vehicle {} outside repair zone at ({:.0f}, {:.0f}, {:.0f})",
                                         vehicle.getID(), position.x, position.y, position.z);
        }
        outcome.vehicleHack = true;
        return outcome;
    }

    // Pay'n'Spray и мод-шоп чинят машину на клиенте НЕЗАВИСИМО от исхода SCM-пакета
    // (встроенное поведение движка GTA:SA в зоне) — sanctionRepair СОХРАНЯЕМ, иначе
    // честный визит перестал бы чинить машину, а следующий driver-sync с полным HP
    // словил бы repair-hack. Сама перекраска — клиентский Pay'n'Spray/мод-шоп больше
    // не источник цвета: отклоняем БЕЗ нарушения (ядровая onVehicleRespray сама не
    // шлёт компенсирующий RPC при false — клиент увидит десинк цвета до следующего
    // полного стрим-ина, см. Docs/Vehicles.md).
    sanctionRepair(vehicle.getID(), timeNow);
    return outcome;
}

VehicleService::Outcome VehicleService::onModShop(IPlayer &player, bool enter, TimePoint timeNow)
{
    Outcome outcome;
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return outcome;
    Occupant &occupant = m_occupants[playerId];
    // Санкция — только серверному факту «за рулём»; ядро SCM не от водителя
    // дропает само, расхождение occupant с ядром чинит реконсиляция verifyHealth.
    if (occupant.seat != 0 || occupant.vehicleId < 0)
        return outcome;
    VehicleState &st = m_vehicleState[occupant.vehicleId];
    if (!st.exists)
        return outcome;
    IVehicle *vehicle = m_vehicles ? m_vehicles->get(occupant.vehicleId) : nullptr;
    if (!vehicle)
        return outcome;

    if (enter)
    {
        // Вход принимаем только с машиной, НЕПРЕРЫВНО простоявшей у ворот
        // известного мод-шопа не меньше ZONE_DWELL_MIN (dwelledInZone — трек
        // verifyHealth на driver-sync): ядро пропускает SCM-событие от любого
        // водителя ГДЕ УГОДНО и зон не знает, а vehicle.getPosition() — клиент-
        // авторитетна (ядро пишет её безусловно из driver-sync). Мгновенная
        // сверка «в зоне сейчас» (nearAnyZone) не отличила бы честный подъезд от
        // суб-порогового телепорт-дрейфа — голый EnterExitModShop давал бы
        // бесплатный серверный ремонт.
        const Vector3 position = vehicle->getPosition();
        if (!dwelledInZone(st, RepairZones::MOD_SHOPS, position, timeNow))
        {
            if (!rateLimited(st.lastFlag, timeNow))
            {
                outcome.detail = fmt::format("mod shop enter for vehicle {} away from shops at ({:.0f}, {:.0f}, {:.0f})",
                                             occupant.vehicleId, position.x, position.y, position.z);
            }
            outcome.vehicleHack = true;
            return outcome;
        }
        occupant.inModShop = true; // серверно-подтверждённая сессия (гейт validateMod/validateRespray)
        sanctionRepair(occupant.vehicleId, timeNow); // шоп чинит машину на клиенте при въезде
        // Клиент физически переносит машину С ВОДИТЕЛЕМ в интерьер шопа
        // (universe-координаты) — позиция игрока скачет на тысячи метров без
        // серверного teleport(). Без грейса PlayerLocationService::verify принял
        // бы честный въезд за телепорт-хак и выдернул бы водителя из шопа
        // forceTo. Грейс — ВРЕМЕННОЕ окно на клиентские заявления позиции (точку
        // прибытия диктует клиент), покрывает всю фазу enter→fade→интерьер.
        if (m_location)
            m_location->grantModShopTeleportGrace(playerId, timeNow);
        return outcome;
    }

    // Выход чинит только сессию, открытую ПРИНЯТЫМ входом. Exit без входа —
    // фейковый SCM (в т.ч. «выход» из шопа, куда игрок не въезжал): без санкции.
    if (!occupant.inModShop)
    {
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = fmt::format("mod shop exit without entry for vehicle {}", occupant.vehicleId);
        }
        outcome.vehicleHack = true;
        return outcome;
    }

    // Выход обязан прийти оттуда же, где живёт сессия: машина ещё в интерьере
    // шопа (universe-координаты в таблице) либо уже у ворот — обе точки в
    // MOD_SHOPS. Exit с живой сессией ВНЕ зоны — клиент подавил честный выход
    // и увёз сессию с собой, чтобы обналичить санкцию ремонта где угодно:
    // сессию закрываем, ремонта не даём. Dwell на ВЫХОД не требуем (мгновенная
    // nearAnyZone): сессия уже прошла dwell-гейт на входе, а между принятым
    // enter и exit машина под тем же driver-sync трека НЕ покидала зону шопа
    // (иначе dwelledInZone уже обнулила бы zoneEnteredAt и следующий вызов
    // validateMod/validateRespray её бы поймал) — повторная выдержка тут
    // избыточна и без выгоды для анти-чита.
    const Vector3 position = vehicle->getPosition();
    if (!nearAnyZone(RepairZones::MOD_SHOPS, position))
    {
        occupant.inModShop = false;
        if (!rateLimited(st.lastFlag, timeNow))
        {
            outcome.detail = fmt::format("mod shop exit for vehicle {} away from shops at ({:.0f}, {:.0f}, {:.0f})",
                                         occupant.vehicleId, position.x, position.y, position.z);
        }
        outcome.vehicleHack = true;
        return outcome;
    }
    occupant.inModShop = false;
    sanctionRepair(occupant.vehicleId, timeNow); // на выезде шоп отдаёт починенную машину
    // Симметрично входу: выезд из интерьера обратно к воротам — тот же
    // клиентский скачок позиции водителя, тот же грейс.
    if (m_location)
        m_location->grantModShopTeleportGrace(playerId, timeNow);
    return outcome;
}

void VehicleService::setEditBypass(int vehicleId, bool enable)
{
    if (vehicleId < 0 || vehicleId >= static_cast<int>(m_vehicleState.size()))
    {
        return;
    }
    m_vehicleState[vehicleId].editBypass = enable;
}

void VehicleService::onVehicleStreamIn(IVehicle &vehicle, IPlayer &player)
{
    for (auto &obs : m_streamedInForPlayerObservers)
        obs(vehicle, player);
}

void VehicleService::onVehicleCreated(IVehicle &vehicle)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st = {}; // сброс: owner=None, бак полон (FUEL_CAPACITY), outOfFuel=false
    st.exists = true;
    st.health = vehicle.getHealth();
    st.lastChange = now();
    // Стейт готов — оповещаем наблюдателей (например, GridSystem добавляет в сетку).
    for (auto &obs : m_createdObservers)
        obs(vehicle);
}

void VehicleService::onVehicleDestroyed(IVehicle &vehicle)
{
    // Наблюдатели вызываются пока машина ещё валидна (до сброса стейта).
    for (auto &obs : m_destroyedObservers)
        obs(vehicle);
    m_vehicleState[vehicle.getID()] = {};
}

void VehicleService::onVehicleRespawn(IVehicle &vehicle)
{
    VehicleState &st = m_vehicleState[vehicle.getID()];
    st.exists = true;
    st.health = MAX_HEALTH;
    st.fuel = FUEL_CAPACITY; // респаун — бак снова полный (как HP)
    st.outOfFuel = false;
    st.serverKilled = false;  // санкция смерти отыграна — новая жизнь машины чиста
    st.respawnQueued = false; // отставший respawnIfDead увидит живую машину и спасует
    st.unsanctionedNotified = false; // машина ожила — следующая смерть уже НОВАЯ волна
    st.lastChange = now();
    // Респаун — СЕРВЕРНЫЙ телепорт на spawn-позицию, а не физический подъезд:
    // если spawn-точка когда-либо совпадёт с ремзоной (конфигурация карты),
    // унаследованный zoneEnteredAt от ПРЕДЫДУЩЕЙ жизни машины (устаревший, но не
    // TimePoint{}) дал бы dwelledInZone мгновенный «зачёт» без единой секунды
    // реального пребывания — сброс закрывает эту щель.
    st.zoneEnteredAt = TimePoint{};
    st.inZoneNow = false;
    clearStall(vehicle, st); // респаун — машина снова целая и заводится
    // owner сохраняется: ту же физическую машину переспавнили — владелец тот же.
    // Ядро уже телепортировало машину на spawn-позицию (pos = spawnData.position до
    // onVehicleSpawn) — getPosition() верный, двигаем пространственный индекс.
    notifyMoved(vehicle, vehicle.getPosition());
    // Дефолты (полный бак/HP) уже применены — наблюдатели могут перезаписать fuel
    // персистентным остатком (setFuel поверх FUEL_CAPACITY). Машина валидна.
    for (auto &obs : m_respawnedObservers)
        obs(vehicle);
}

VehicleService::Outcome VehicleService::onVehicleDeath(IVehicle &vehicle, IPlayer &reporter)
{
    Outcome outcome;
    VehicleState &st = m_vehicleState[vehicle.getID()];
    const TimePoint timeNow = now();
    st.health = 0.0f;
    st.lastChange = timeNow;

    if (st.serverKilled)
    {
        // Санкционированная смерть (explode) — машина ещё валидна, оповещаем
        // died-наблюдателей: политику решает бизнес (напр., убрать личную машину,
        // чтобы она не висела вреком). Машина может быть залочена в пуле во время
        // диспатча смерти: destroy() (release) из наблюдателя отложится до unlock —
        // диспатч смерти не рвётся. Репортер отыграл серверный сценарий — в темп
        // репортов не считается.
        for (auto &obs : m_diedObservers)
            obs(vehicle);
        return outcome;
    }

    // Смерть БЕЗ серверной санкции (клиентская детонация пустой: расстрел,
    // утопление, взрывчатка — либо фейковый репорт: VehicleDeath RPC, driver-sync
    // или unoccupied-синк с Health<=0): died-политика НЕ применяется — бизнес не
    // теряет машину по слову чужого клиента.
    //
    // САМ репорт не флажим: сервер урона по пустой машине не видит (HP из
    // unoccupied-синка ядро пишет только от пассажира, стрельбу по пустым
    // onPlayerShotVehicle пропускает) — честная детонация по серверным фактам
    // неотличима от фейка. Но ТЕМП репортов — серверный факт: человек не сносит
    // машины пачками окно за окном. Скользящее окно на репортера; запись
    // нарушения — под rate limit.
    const int reporterId = reporter.getID();
    if (reporterId >= 0 && reporterId < MAX_PLAYERS)
    {
        DeathReportRate &rate = m_deathReports[reporterId];
        if (timeNow - rate.windowStart > DEATH_REPORT_WINDOW)
        {
            rate.windowStart = timeNow;
            rate.count = 0;
        }
        ++rate.count;
        if (rate.count > DEATH_REPORT_MAX && !rateLimited(rate.lastFlag, timeNow))
        {
            outcome.vehicleHack = true;
            outcome.detail = fmt::format("vehicle death reports: {} in {}s", rate.count,
                                         DEATH_REPORT_WINDOW.count());
        }
    }

    // Возврат: ПЕРВАЯ смерть бэкофф-окна — быстрый респавн НА МЕСТЕ (queueRespawn
    // -> respawnIfDead с точкой смерти, зафиксированной здесь): грифер не получает
    // ни потери машины, ни телепорта («эвакуатор домой» закрыт). ПОВТОРНАЯ смерть
    // в окне (после возврата умерла снова: утопленная либо спам-грифинг) наш
    // респавн не взводит — машину вернёт ядровой death-таймер на её spawn-точку
    // (~10 с). Явный гейт lastUnsanctionedDeath != TimePoint(): первая смерть на
    // свежем сервере/машине не должна ложно попасть в «повтор». queueRespawn
    // взводится один раз на волну смертей (спам setDead диспатчит смерть каждый
    // тик — таймеры не плодим).
    const bool repeatDeath = st.lastUnsanctionedDeath != TimePoint() &&
                             timeNow - st.lastUnsanctionedDeath < UNSANCTIONED_DEATH_BACKOFF;
    st.lastUnsanctionedDeath = timeNow;
    bool queuedThisCall = false;
    if (!repeatDeath && !st.respawnQueued)
    {
        // Точка возврата — позиция/угол в момент фиксации смерти сервером
        // (принятые валидаторами координаты): окно до таймера не даёт дотащить
        // врек дальше синками.
        st.deathPos = vehicle.getPosition();
        st.deathAngle = vehicle.getZAngle();
        st.respawnQueued = true;
        outcome.queueRespawn = true;
        queuedThisCall = true;
    }

    // Оповещаем бизнес о ФАКТЕ несанкционированной смерти — ПОСЛЕ решения о
    // возврате, машина ещё валидна (died-lock диспатча смерти держит её живой,
    // см. respawnIfDead/died→destroy→destroyed в доках). returnsInPlace ==
    // queuedThisCall: true — respawnIfDead вернёт НА МЕСТЕ (~100 мс), false —
    // повторная смерть окна (respawnQueued уже отыгран прошлым respawnIfDead) —
    // машину вернёт ядровой death-таймер на spawn-точку (~10 с).
    //
    // Антиспам — ОДНА строка на волну детонаций: unsanctionedNotified взводится
    // здесь на ПЕРВОМ вызове волны (машина только что умерла — либо свежая
    // смерть после предыдущего респавна, либо первая на сервере) и снимается
    // ТОЛЬКО respawn'ом (onVehicleRespawn). Пока машина мертва, читер может
    // слать setDead каждый sync, а ядро диспатчит onVehicleDeath на каждом
    // onTick с новым deathData.time (vehicles_impl.hpp) — без этого флага
    // колбэк дублировался бы на каждый такой вызов, включая ВСЁ ожидание
    // ядрового ~10-с таймера на повторной смерти. Флаг не таймерный
    // (в отличие от respawnQueued/бэкоффа) — привязан к самой мёртвой фазе,
    // поэтому корректно накрывает и «на месте», и «повторная».
    if (!st.unsanctionedNotified)
    {
        st.unsanctionedNotified = true;
        for (auto &obs : m_unsanctionedDeathObservers)
            obs(vehicle, queuedThisCall);
    }
    return outcome;
}

void VehicleService::respawnIfDead(int vehicleId)
{
    if (vehicleId < 0 || vehicleId >= VEHICLE_POOL_SIZE || !m_vehicles)
        return;
    VehicleState &st = m_vehicleState[vehicleId];
    st.respawnQueued = false; // окно закрыто: следующая волна смертей взведёт новый таймер
    if (!st.exists)
        return;
    IVehicle *vehicle = m_vehicles->get(vehicleId);
    if (!vehicle)
        return; // уничтожена, пока таймер ждал
    // Не мертва — слот успели пересоздать либо машину уже вернул кто-то ещё
    // (ядро само так быстро не вернёт: перед проверкой death-таймера оно
    // поднимает lastOccupiedTime до момента смерти, same-tick респавна не
    // бывает — пустую возвращает именно этот таймер). Занятую (читер-пассажир
    // в вреке) не респавним, как и ядро, — её вернёт ядровой death-таймер
    // после освобождения.
    if (!vehicle->isDead() || vehicle->isOccupied())
        return;
    // Возврат НА МЕСТЕ: SDK не умеет снять deathData без respawn(), поэтому
    // spawn-точка на время respawn() подменяется точкой смерти (копия spawnData,
    // меняются только позиция/угол) и сразу восстанавливается — репортер смерти
    // не работает «эвакуатором» чужих машин, а санкционированный/ядровой респавн
    // по-прежнему ведёт на родную точку. Стрим-аут/ин респавна неустраним — он же
    // и чистит врек у клиентов. Мусорная точка смерти (валидаторы синков её не
    // пропускают — чистая страховка) — фолбэк на родную spawn-точку.
    const VehicleSpawnData original = vehicle->getSpawnData(); // const& -> локальная КОПИЯ
    const bool inPlace = std::isfinite(st.deathPos.x) && std::isfinite(st.deathPos.y) &&
                         std::isfinite(st.deathPos.z) && std::isfinite(st.deathAngle);
    if (inPlace)
    {
        VehicleSpawnData atDeath = original;
        atDeath.position = st.deathPos;
        atDeath.zRotation = st.deathAngle;
        vehicle->setSpawnData(atDeath);
    }
    // respawn() ядра: стрим-аут, deathData сброшена (второго, ядрового респавна не
    // будет), pos = подставленная точка смерти; затем onVehicleSpawn ->
    // onVehicleRespawn — HP/бак/стейт/грид обновятся штатным путём.
    vehicle->respawn();
    if (inPlace)
        vehicle->setSpawnData(original);
}

void VehicleService::resetPlayer(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    const Occupant &occupant = m_occupants[playerId];
    if (occupant.seat == 0 && occupant.vehicleId >= 0 && m_vehicleState[occupant.vehicleId].driverId == playerId)
    {
        m_vehicleState[occupant.vehicleId].driverId = -1;
    }
    m_occupants[playerId] = {};
    // Окно темп-детектора смертей — тоже сбросить: иначе новый игрок в
    // переиспользованном слоте наследует счётчик прежнего (ложный флаг/подавление).
    m_deathReports[playerId] = {};
}
