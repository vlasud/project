#include "Systems/VehicleWaypointSystem/VehicleWaypointSystem.h"

VehicleWaypointSystem::VehicleWaypointSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);

    m_waypointService.bind(serviceRegister.getService<CheckpointService>());

    // Машину-цель могли уничтожить (взрыв/destroy/пере-спавн старого экземпляра) —
    // если у кого-то на неё стоит указатель, снимаем его (не оставляем висеть
    // маркером на пропавшую машину). Живой игрок -> clearFor (гасит и клиентский
    // чекпоинт); вышел -> resetPlayer (только обнулить цель, слот уже сбросил
    // CheckpointService на его дисконнекте).
    m_vehicleService.subscribeDestroyed(
        [this](IVehicle &vehicle)
        {
            const int playerId = m_waypointService.findByVehicle(vehicle.getID());
            if (playerId < 0)
            {
                return;
            }
            if (IPlayer *player = m_core.getPlayers().get(playerId))
            {
                m_waypointService.clearFor(*player);
            }
            else
            {
                m_waypointService.resetPlayer(playerId);
            }
        });
}

void VehicleWaypointSystem::onPlayerConnect(IPlayer &player)
{
    // Чистый старт слота (на случай незасланного дисконнекта прежнего владельца
    // слота). Клиентский чекпоинт пересоздаст setForPlayer при следующей подаче.
    m_waypointService.resetPlayer(player.getID());
}

void VehicleWaypointSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason /*reason*/)
{
    // Гасим только нашу цель — клиентский чекпоинт CheckpointService сбросит сам.
    m_waypointService.resetPlayer(player.getID());
}
