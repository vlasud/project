#include "Systems/Core/GridSystem/GridSystem.h"

GridSystem::GridSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_gridService(serviceRegister.getService<GridService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>())
{
    m_playerHandles.fill(GridService::INVALID_HANDLE);
    m_vehicleHandles.fill(GridService::INVALID_HANDLE);

    listen(core.getPlayers().getPlayerUpdateDispatcher(), this);
    listen(core.getPlayers().getPlayerSpawnDispatcher(), this);
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

void GridSystem::initialize(IComponentList *components)
{
    (void)components;
    m_vehicleService.subscribeCreated([this](IVehicle &vehicle) { onVehicleAdded(vehicle); });
    m_vehicleService.subscribeDestroyed([this](IVehicle &vehicle) { onVehicleRemoved(vehicle); });
    m_vehicleService.subscribeMoved([this](IVehicle &vehicle, Vector3 position) { onVehicleMoved(vehicle, position); });
}

bool GridSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    const int playerId = player.getID();
    GridService::Handle &handle = m_playerHandles[playerId];
    if (handle == GridService::INVALID_HANDLE)
    {
        return true; // ещё не заспавнен
    }

    // Принятая сервером позиция, а не сырая клиентская: читерский скачок,
    // откаченный валидатором, в сетку не попадает.
    m_gridService.move(handle, m_locationService.getPosition(playerId));

    // Машина едет только когда её синхронизирует водитель — обновляем её здесь же.
    // Машина игрока — из источника правды (заодно без queryExtension на апдейт).
    if (m_vehicleService.getSeat(playerId) == 0)
    {
        IVehicle *vehicle = m_vehicleService.getVehicle(playerId);
        if (vehicle)
        {
            const GridService::Handle vehicleHandle = m_vehicleHandles[vehicle->getID()];
            if (vehicleHandle != GridService::INVALID_HANDLE)
            {
                m_gridService.move(vehicleHandle, vehicle->getPosition());
            }
        }
    }

    return true;
}

void GridSystem::onPlayerSpawn(IPlayer &player)
{
    // LocationSystem обрабатывает спавн раньше (порядок регистрации) — позиция в
    // сервисе уже принята.
    const Vector3 position = m_locationService.getPosition(player.getID());
    GridService::Handle &handle = m_playerHandles[player.getID()];
    if (handle == GridService::INVALID_HANDLE)
    {
        handle = m_gridService.add(GridEntityType::Player, player.getID(), position);
    }
    else
    {
        m_gridService.move(handle, position); // респаун
    }
}

void GridSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    GridService::Handle &handle = m_playerHandles[player.getID()];
    if (handle != GridService::INVALID_HANDLE)
    {
        m_gridService.remove(handle);
        handle = GridService::INVALID_HANDLE;
    }
}

void GridSystem::onVehicleAdded(IVehicle &vehicle)
{
    m_vehicleHandles[vehicle.getID()] = m_gridService.add(GridEntityType::Vehicle, vehicle.getID(), vehicle.getPosition());
}

void GridSystem::onVehicleRemoved(IVehicle &vehicle)
{
    GridService::Handle &handle = m_vehicleHandles[vehicle.getID()];
    if (handle != GridService::INVALID_HANDLE)
    {
        m_gridService.remove(handle);
        handle = GridService::INVALID_HANDLE;
    }
}

void GridSystem::onVehicleMoved(IVehicle &vehicle, Vector3 acceptedPosition)
{
    const int vehicleId = vehicle.getID();
    if (vehicleId < 0 || vehicleId >= static_cast<int>(m_vehicleHandles.size()))
        return;
    const GridService::Handle handle = m_vehicleHandles[vehicleId];
    if (handle != GridService::INVALID_HANDLE)
    {
        // Позиция уже принята сервером (валидатор unoccupied / телепорт на спавн) —
        // кладём как есть. NaN/Inf в координате гасит сам GridService (cellCoord).
        m_gridService.move(handle, acceptedPosition);
    }
}
