#include "Services/VehicleWaypointService/VehicleWaypointService.h"

void VehicleWaypointService::bind(CheckpointService &checkpoints)
{
    m_checkpoints = &checkpoints;
}

void VehicleWaypointService::showFor(IPlayer &player, IVehicle &vehicle)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    if (!m_checkpoints)
    {
        return; // сервис не связан — чекпоинт ставить нечем
    }

    // Позицию машины берёт СЕРВЕР (getPosition), не клиент. Пере-вызов заменяет
    // прежний указатель: setForPlayer перерисует слот. Вход в чекпоинт снимает его
    // сам (onEnter -> clearFor) — снятие из onEnter безопасно (CheckpointService
    // копирует обработчик до вызова, нет ре-энтрантной порчи слота).
    m_targets[playerId].vehicleId = vehicle.getID();
    m_checkpoints->setForPlayer(player, vehicle.getPosition(), CHECKPOINT_RADIUS,
                                [this](IPlayer &p) { clearFor(p); });
}

void VehicleWaypointService::clearFor(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    if (m_targets[playerId].vehicleId == -1)
    {
        return; // указателя нет — снимать нечего
    }
    m_targets[playerId].vehicleId = -1;
    if (m_checkpoints)
    {
        m_checkpoints->clearForPlayer(player);
    }
}

void VehicleWaypointService::resetPlayer(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_targets[playerId].vehicleId = -1;
}

int VehicleWaypointService::findByVehicle(int vehicleId) const
{
    if (vehicleId == -1)
    {
        return -1; // -1 значит «нет цели» — не матчим пустые слоты
    }
    for (int playerId = 0; playerId < MAX_PLAYERS; ++playerId)
    {
        if (m_targets[playerId].vehicleId == vehicleId)
        {
            return playerId;
        }
    }
    return -1;
}

bool VehicleWaypointService::hasWaypoint(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return false;
    }
    return m_targets[playerId].vehicleId != -1;
}
