#include "VehicleSystem.h"

#include "../../Services/PlayerLocationService/PlayerLocationService.h"
#include "../PlayerHealthSystem/WeaponLimits.h" // табличный урон оружия
#include <chrono>

namespace
{
TimePoint now()
{
    return std::chrono::steady_clock::now();
}
} // namespace

VehicleSystem::VehicleSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>())
{
    core.getPlayers().getPlayerChangeDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void VehicleSystem::initialize(IComponentList *components)
{
    m_vehicles = components->queryComponent<IVehiclesComponent>();
    m_vehicleService.bind(m_vehicles, m_serviceRegister.getService<PlayerLocationService>());
    if (m_vehicles)
    {
        m_vehicles->getEventDispatcher().addEventHandler(this);
        m_vehicles->getPoolEventDispatcher().addEventHandler(this);
    }

    // Подписка на выстрелы именно здесь (initialize выполняется после всех
    // конструкторов): мы оказываемся в диспатчере ПОСЛЕ PlayerWeaponSystem —
    // фейковый выстрел из невыданного оружия отброшен до нас и урона не нанесёт.
    m_core.getPlayers().getPlayerShotDispatcher().addEventHandler(this);
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

void VehicleSystem::onVehicleDeath(IVehicle &vehicle, IPlayer &player)
{
    m_vehicleService.onVehicleDeath(vehicle);
}

void VehicleSystem::onPlayerEnterVehicle(IPlayer &player, IVehicle &vehicle, bool passenger)
{
    // Начало входа (нажатие Enter у двери) — сигнал фазы для валидатора стейта:
    // легальная посадка приходит как OnFoot -> Driver без промежуточных стейтов.
    m_stateService.onEnterVehicle(player, vehicle.getID(), now());
}

bool VehicleSystem::onVehicleMod(IPlayer &player, IVehicle &vehicle, int component)
{
    VehicleService::Outcome outcome = m_vehicleService.validateMod(player, vehicle, component, now());
    if (outcome.vehicleHack)
    {
        record(player, outcome);
        return false; // мод вне мод-шопа / не водителем ядро не применит
    }
    return true;
}

bool VehicleSystem::onVehicleRespray(IPlayer &player, IVehicle &vehicle, int colour1, int colour2)
{
    VehicleService::Outcome outcome = m_vehicleService.validateRespray(player, vehicle, now());
    if (outcome.vehicleHack)
    {
        record(player, outcome);
        return false;
    }
    return true;
}

void VehicleSystem::onEnterExitModShop(IPlayer &player, bool enterexit, int interiorID)
{
    m_vehicleService.onModShop(player, now());
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
