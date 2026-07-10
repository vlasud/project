#include "Services/VehicleWaypointService/VehicleWaypointService.h"

#include <utility>

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
    m_targets[playerId] = {vehicle.getID(), true};
    m_checkpoints->setForPlayer(player, vehicle.getPosition(), CHECKPOINT_RADIUS,
                                [this](IPlayer &p) { clearFor(p); });
}

void VehicleWaypointService::showFor(IPlayer &player, const Vector3 &point)
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

    // Цель-точка: vehicleId=-1 (findByVehicle её никогда не матчит — нет живого
    // экземпляра, который могут уничтожить). Пере-вызов (в т.ч. showFor к машине)
    // заменяет прежнюю цель тем же единым слотом.
    m_targets[playerId] = {-1, true};
    m_checkpoints->setForPlayer(player, point, CHECKPOINT_RADIUS, [this](IPlayer &p) { clearFor(p); });
}

void VehicleWaypointService::showGpsFor(IPlayer &player, const Vector3 &point, std::function<void(IPlayer &)> onArrive)
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

    // GPS-цель: тот же единый слот, что и парковка/дом, но помечена gps=true
    // (vehicleId=-1 — findByVehicle её не матчит). Вход снимает маркер сам (clearFor),
    // затем зовём onArrive — сообщение о прибытии. Снятие из onEnter безопасно
    // (CheckpointService копирует обработчик до вызова).
    m_targets[playerId] = {-1, true, true};
    m_checkpoints->setForPlayer(player, point, CHECKPOINT_RADIUS,
                                [this, onArrive = std::move(onArrive)](IPlayer &p)
                                {
                                    clearFor(p);
                                    if (onArrive)
                                    {
                                        onArrive(p);
                                    }
                                });
}

void VehicleWaypointService::clearFor(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    if (!m_targets[playerId].hasTarget)
    {
        return; // указателя нет — снимать нечего
    }
    m_targets[playerId] = {};
    if (m_checkpoints)
    {
        m_checkpoints->clearForPlayer(player);
    }
}

void VehicleWaypointService::clearGpsFor(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    // Гасим только GPS-цель — парковочный/домашний указатель не трогаем.
    if (m_targets[playerId].hasTarget && m_targets[playerId].gps)
    {
        clearFor(player);
    }
}

bool VehicleWaypointService::hasGpsWaypoint(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return false;
    }
    return m_targets[playerId].hasTarget && m_targets[playerId].gps;
}

void VehicleWaypointService::resetPlayer(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_targets[playerId] = {};
}

int VehicleWaypointService::findByVehicle(int vehicleId) const
{
    if (vehicleId == -1)
    {
        return -1; // -1 значит «нет цели» либо цель-точка — не матчим ни то ни другое
    }
    for (int playerId = 0; playerId < MAX_PLAYERS; ++playerId)
    {
        if (m_targets[playerId].hasTarget && m_targets[playerId].vehicleId == vehicleId)
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
    return m_targets[playerId].hasTarget;
}
