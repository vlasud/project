#include "Systems/Core/VehicleSystem/VehicleSystem.h"

#include "Services/Core/GridService/GridService.h" // пробрасываем в VehicleService::bind
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Systems/Core/PlayerHealthSystem/WeaponLimits.h" // табличный урон оружия
#include <chrono>

namespace
{
TimePoint now()
{
    return std::chrono::steady_clock::now();
}

// Задержка серверного респавна после НЕсанкционированной смерти машины: вне
// death-диспатча ядра (см. onVehicleDeath), но намного раньше ядрового
// game.vehicle_respawn_time (10 с) — врек не успевает повисеть.
constexpr Milliseconds UNSANCTIONED_DEATH_RESPAWN_DELAY{100};
} // namespace

VehicleSystem::VehicleSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>()),
      m_timerService(serviceRegister.getService<TimerService>())
{
    listen(core.getPlayers().getPlayerChangeDispatcher(), this);
    listen(core.getPlayers().getPlayerUpdateDispatcher(), this);
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

void VehicleSystem::initialize(IComponentList *components)
{
    // Единственное место queryComponent<IVehiclesComponent> в геймоде (DI-handoff):
    // результат немедленно уходит в bind и нигде не сохраняется. GridService к этому
    // моменту сконструирован (все сервисы — до фазы initialize), хоть в реестре он и
    // после VehicleService: anyVehicleNear спрашивает соседей-машин у него.
    m_vehicleService.bind(components->queryComponent<IVehiclesComponent>(),
                          m_serviceRegister.getService<PlayerLocationService>(),
                          m_serviceRegister.getService<GridService>(), *this, *this);

    // Подписка на выстрелы именно здесь (initialize выполняется после всех
    // конструкторов): мы оказываемся в диспатчере ПОСЛЕ PlayerWeaponSystem —
    // фейковый выстрел из невыданного оружия отброшен до нас и урона не нанесёт.
    listen(m_core.getPlayers().getPlayerShotDispatcher(), this);

    // Секундный проход по пулу — по таймеру (после bind: пул машин готов), не
    // per-tick: дренаж топлива + тушение машин без водителя (их HP пишут принятые
    // unoccupied-синки пассажира мимо verifyHealth — см. secondTick). Колбэк на
    // главном потоке.
    m_timerService.setInterval(Seconds{1}, [this]() { m_vehicleService.secondTick(1.0f); });
}

bool VehicleSystem::onPlayerShotVehicle(IPlayer &player, IVehicle &target, const PlayerBulletData &bulletData)
{
    // При lagcomp пули симулируются на клиенте стрелка — клиент водителя чужих
    // попаданий не видит и урона своей машине не насчитает. Урон от стрельбы по
    // машине с водителем применяет сервер; честный водитель примет setHealth
    // (грейс), god-mode водитель разойдётся и попадётся repair-детектору.
    // Пустые машины не трогаем: их повреждения — клиентская симуляция как есть.
    if (m_vehicleService.getDriver(target.getID()) < 0)
    {
        return true;
    }

    const WeaponLimits::Info *info = WeaponLimits::get(bulletData.weapon);
    if (info)
    {
        m_vehicleService.applyDamage(target, info->maxDamage);
    }
    return true;
}

void VehicleSystem::record(IPlayer &player, VehicleService::Outcome &outcome)
{
    if (outcome.vehicleHack && !outcome.detail.empty())
    {
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::VehicleHack,
                                  std::move(outcome.detail), now());
    }
}

void VehicleSystem::onPlayerStateChange(IPlayer &player, PlayerState newState, PlayerState oldState)
{
    m_vehicleService.bindOccupant(player, newState);
}

bool VehicleSystem::onPlayerUpdate(IPlayer &player, TimePoint timeNow)
{
    VehicleService::Outcome outcome = m_vehicleService.verifyHealth(player, timeNow);
    record(player, outcome);
    return true;
}

void VehicleSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_vehicleService.resetPlayer(player.getID());
}

void VehicleSystem::onVehicleSpawn(IVehicle &vehicle)
{
    m_vehicleService.onVehicleRespawn(vehicle);
}

void VehicleSystem::onVehicleStreamIn(IVehicle &vehicle, IPlayer &player)
{
    m_vehicleService.onVehicleStreamIn(vehicle, player);
}

void VehicleSystem::onVehicleDeath(IVehicle &vehicle, IPlayer &reporter)
{
    // Сам репорт смерти нарушением не считается (честная клиентская детонация
    // пустой машины неотличима от фейка), но НЕЧЕЛОВЕЧЕСКИЙ темп репортов одного
    // игрока сервис флажит — записываем как обычное нарушение.
    VehicleService::Outcome outcome = m_vehicleService.onVehicleDeath(vehicle, reporter);
    record(reporter, outcome);
    if (outcome.queueRespawn)
    {
        // Несанкционированная смерть: вернуть машину целой НА МЕСТЕ смерти.
        // respawn() ПРЯМО из death-диспатча нельзя: ядро после диспатча сверяет
        // now - lastOccupiedTime >= game.vehicle_respawn_time, а _respawn()
        // обнуляет lastOccupiedTime — условие стало бы истинным, и ядро тут же
        // респавнило бы ВТОРОЙ раз. Откладываем на таймер вне диспатча;
        // respawnIfDead респавнит, только если машина всё ещё мертва и пуста.
        const int vehicleId = vehicle.getID();
        m_timerService.setTimeout(UNSANCTIONED_DEATH_RESPAWN_DELAY,
                                  [this, vehicleId]() { m_vehicleService.respawnIfDead(vehicleId); });
    }
}

void VehicleSystem::onPlayerEnterVehicle(IPlayer &player, IVehicle &vehicle, bool passenger)
{
    // Начало входа (нажатие Enter у двери) — сигнал фазы для валидатора стейта:
    // легальная посадка приходит как OnFoot -> Driver без промежуточных стейтов.
    m_stateService.onEnterVehicle(player, vehicle.getID(), now());
}

bool VehicleSystem::onVehicleMod(IPlayer &player, IVehicle &vehicle, int component)
{
    // Клиентский мод-гараж больше НЕ источник тюнинга: ядро НИКОГДА не применяет
    // клиентскую заявку. record не ложно-банит легитимного игрока — outcome.vehicleHack
    // остаётся false (и detail пуст) для честного визита мод-шопа (см. validateMod);
    // читерская заявка (не водитель/битый id/вне зоны) по-прежнему пишет нарушение.
    VehicleService::Outcome outcome = m_vehicleService.validateMod(player, vehicle, component, now());
    record(player, outcome);
    return false;
}

bool VehicleSystem::onVehiclePaintJob(IPlayer &player, IVehicle &vehicle, int paintJob)
{
    // См. onVehicleMod — клиентский пейнтджоб тоже больше не источник.
    VehicleService::Outcome outcome = m_vehicleService.validatePaintJob(player, vehicle, paintJob, now());
    record(player, outcome);
    return false;
}

bool VehicleSystem::onVehicleRespray(IPlayer &player, IVehicle &vehicle, int colour1, int colour2)
{
    // См. onVehicleMod — клиентская перекраска тоже больше не источник (репейр
    // Pay'n'Spray/мод-шопа validateRespray сохраняет отдельно от исхода цвета).
    VehicleService::Outcome outcome = m_vehicleService.validateRespray(player, vehicle, now());
    record(player, outcome);
    return false;
}

void VehicleSystem::onEnterExitModShop(IPlayer &player, bool enterexit, int interiorID)
{
    // Ядро возврат не читает (диспатч .all) — эффект гейта в том, что фейковое
    // событие не получает санкции ремонта; нарушение уходит в журнал.
    VehicleService::Outcome outcome = m_vehicleService.onModShop(player, enterexit, now());
    record(player, outcome);
}

bool VehicleSystem::onUnoccupiedVehicleUpdate(IVehicle &vehicle, IPlayer &player, UnoccupiedVehicleUpdate const updateData)
{
    VehicleService::Outcome outcome = m_vehicleService.validateUnoccupied(vehicle, player, updateData, now());
    if (outcome.vehicleHack)
    {
        record(player, outcome);
        return false; // фейковый апдейт пустой машины ядро не применит
    }
    return true;
}

bool VehicleSystem::onTrailerUpdate(IPlayer &player, IVehicle &trailer)
{
    VehicleService::Outcome outcome = m_vehicleService.validateTrailer(player, trailer, now());
    if (outcome.vehicleHack)
    {
        record(player, outcome);
        return false;
    }
    return true;
}

void VehicleSystem::onPoolEntryCreated(IVehicle &vehicle)
{
    m_vehicleService.onVehicleCreated(vehicle);
}

void VehicleSystem::onPoolEntryDestroyed(IVehicle &vehicle)
{
    m_vehicleService.onVehicleDestroyed(vehicle);
}
