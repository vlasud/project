#include "Services/Core/SpectateService/SpectateService.h"

#include "Log/LogManager.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "core.hpp"
#include <cmath>

namespace
{
// Тело наблюдателя держим возле цели (иначе цель выйдет из стрима и камера
// замрёт); подтягиваем только при заметном отрыве, чтобы не спамить телепорты.
constexpr float FOLLOW_DISTANCE = 100.0f;

float distanceSq(const Vector3 &a, const Vector3 &b)
{
    const Vector3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}
} // namespace

bool SpectateService::spectatePlayer(IPlayer &spectator, IPlayer &target, StopHandler onStop)
{
    if (spectator.getID() == target.getID())
    {
        return false;
    }

    Slot &slot = m_slots[spectator.getID()];
    if (!slot.active)
    {
        beginSession(spectator);
    }
    slot.vehicleTarget = false;
    slot.targetId = target.getID();
    slot.onStop = std::move(onStop);

    applySpectate(spectator, slot);
    return true;
}

bool SpectateService::spectateVehicle(IPlayer &spectator, IVehicle &target, StopHandler onStop)
{
    Slot &slot = m_slots[spectator.getID()];
    if (!slot.active)
    {
        beginSession(spectator);
    }
    slot.vehicleTarget = true;
    slot.targetId = target.getID();
    slot.onStop = std::move(onStop);

    applySpectate(spectator, slot);
    return true;
}

void SpectateService::stop(IPlayer &spectator)
{
    stopInternal(spectator, StopReason::Manual);
}

bool SpectateService::isSpectating(int playerId) const
{
    if (!validPlayerId(playerId))
        return false;
    return m_slots[playerId].active;
}

int SpectateService::spectateTargetPlayer(int playerId) const
{
    if (!validPlayerId(playerId))
        return -1;
    const Slot &slot = m_slots[playerId];
    return (slot.active && !slot.vehicleTarget) ? slot.targetId : -1;
}

// ------------------------------------------------------------------ вызовы SpectateSystem

void SpectateService::initialize(ICore *core, PlayerStateService *state, PlayerLocationService *location,
                                 VehicleService *vehicleService)
{
    m_core = core;
    m_state = state;
    m_location = location;
    m_vehicleService = vehicleService;
}

void SpectateService::sweep()
{
    if (!m_core || !m_location)
    {
        return;
    }

    for (int spectatorId = 0; spectatorId < MAX_PLAYERS; ++spectatorId)
    {
        Slot &slot = m_slots[spectatorId];
        if (!slot.active)
        {
            continue;
        }
        IPlayer *spectator = m_core->getPlayers().get(spectatorId);
        if (!spectator)
        {
            continue; // дисконнект почистит resetPlayer
        }

        // Цель ещё существует? Догоняем её интерьер/мир/позицию тела.
        Vector3 targetPos{};
        int targetWorld = 0;
        unsigned targetInterior = 0;

        if (slot.vehicleTarget)
        {
            IVehicle *vehicle = m_vehicleService ? m_vehicleService->get(slot.targetId) : nullptr;
            if (!vehicle)
            {
                stopInternal(*spectator, StopReason::TargetLost);
                continue;
            }
            targetPos = vehicle->getPosition();
            targetWorld = vehicle->getVirtualWorld();
            targetInterior = static_cast<unsigned>(vehicle->getInterior());
        }
        else
        {
            IPlayer *target = m_core->getPlayers().get(slot.targetId);
            if (!target)
            {
                stopInternal(*spectator, StopReason::TargetLost);
                continue;
            }
            targetPos = m_location->getPosition(slot.targetId);
            targetWorld = m_location->getVirtualWorld(slot.targetId);
            targetInterior = m_location->getInterior(slot.targetId);
        }

        if (m_location->getVirtualWorld(spectatorId) != targetWorld)
        {
            m_location->setVirtualWorld(*spectator, targetWorld);
        }
        if (m_location->getInterior(spectatorId) != targetInterior)
        {
            m_location->setInterior(*spectator, targetInterior);
        }
        if (distanceSq(m_location->getPosition(spectatorId), targetPos) > FOLLOW_DISTANCE * FOLLOW_DISTANCE)
        {
            m_location->teleport(*spectator, targetPos);
        }
    }
}

void SpectateService::handleSpawn(IPlayer &player)
{
    const int playerId = player.getID();

    // Возврат наблюдателя: выход из спектейта — это респаун.
    Slot &slot = m_slots[playerId];
    if (slot.pendingReturn)
    {
        slot.pendingReturn = false;
        if (m_location)
        {
            m_location->teleport(player, slot.returnPosition);
            m_location->setInterior(player, slot.returnInterior);
            m_location->setVirtualWorld(player, slot.returnWorld);
        }
        return;
    }

    // Заспавнился чей-то таргет: клиентский спектейт слетел — переприменяем.
    if (!m_core)
    {
        return;
    }
    for (int spectatorId = 0; spectatorId < MAX_PLAYERS; ++spectatorId)
    {
        Slot &watcher = m_slots[spectatorId];
        if (watcher.active && !watcher.vehicleTarget && watcher.targetId == playerId)
        {
            if (IPlayer *spectator = m_core->getPlayers().get(spectatorId))
            {
                applySpectate(*spectator, watcher);
            }
        }
    }
}

void SpectateService::resetPlayer(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    m_slots[playerId] = Slot{};

    // Он мог быть чьей-то целью — наблюдателей останавливаем с TargetLost.
    if (!m_core)
    {
        return;
    }
    for (int spectatorId = 0; spectatorId < MAX_PLAYERS; ++spectatorId)
    {
        Slot &watcher = m_slots[spectatorId];
        if (watcher.active && !watcher.vehicleTarget && watcher.targetId == playerId)
        {
            if (IPlayer *spectator = m_core->getPlayers().get(spectatorId))
            {
                stopInternal(*spectator, StopReason::TargetLost);
            }
            else
            {
                m_slots[spectatorId] = Slot{};
            }
        }
    }
}

// ------------------------------------------------------------------ private

void SpectateService::beginSession(IPlayer &spectator)
{
    const int spectatorId = spectator.getID();
    Slot &slot = m_slots[spectatorId];

    slot.active = true;
    slot.pendingReturn = false;
    if (m_location)
    {
        slot.returnPosition = m_location->getPosition(spectatorId);
        slot.returnInterior = m_location->getInterior(spectatorId);
        slot.returnWorld = m_location->getVirtualWorld(spectatorId);
    }
    if (m_state)
    {
        m_state->setSpectating(spectator, true);
    }
}

void SpectateService::applySpectate(IPlayer &spectator, Slot &slot)
{
    if (slot.vehicleTarget)
    {
        IVehicle *vehicle = m_vehicleService ? m_vehicleService->get(slot.targetId) : nullptr;
        if (!vehicle)
        {
            stopInternal(spectator, StopReason::TargetLost);
            return;
        }
        if (m_location)
        {
            m_location->setVirtualWorld(spectator, vehicle->getVirtualWorld());
            m_location->setInterior(spectator, static_cast<unsigned>(vehicle->getInterior()));
            m_location->teleport(spectator, vehicle->getPosition());
        }
        spectator.spectateVehicle(*vehicle, PlayerSpectateMode_Normal);
        return;
    }

    IPlayer *target = m_core ? m_core->getPlayers().get(slot.targetId) : nullptr;
    if (!target)
    {
        stopInternal(spectator, StopReason::TargetLost);
        return;
    }
    if (m_location)
    {
        m_location->setVirtualWorld(spectator, m_location->getVirtualWorld(slot.targetId));
        m_location->setInterior(spectator, m_location->getInterior(slot.targetId));
        m_location->teleport(spectator, m_location->getPosition(slot.targetId));
    }
    spectator.spectatePlayer(*target, PlayerSpectateMode_Normal);
}

void SpectateService::stopInternal(IPlayer &spectator, StopReason reason)
{
    Slot &slot = m_slots[spectator.getID()];
    if (!slot.active)
    {
        return;
    }

    slot.active = false;
    slot.targetId = -1;
    slot.pendingReturn = true; // респаун вернёт на место в handleSpawn

    // Колбэк забираем до вызова: он может сразу начать новый спектейт.
    StopHandler onStop = std::move(slot.onStop);
    slot.onStop = nullptr;

    if (m_state)
    {
        m_state->setSpectating(spectator, false);
    }
    if (onStop)
    {
        onStop(spectator, reason);
    }
}
