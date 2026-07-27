#include "Systems/Core/PlayerStateSystem/PlayerStateSystem.h"

#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include <chrono>

PlayerStateSystem::PlayerStateSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>())
{
    m_stateService.bind(serviceRegister.getService<PlayerLocationService>());

    listen(core.getPlayers().getPlayerChangeDispatcher(), this);
    listen(core.getPlayers().getPlayerUpdateDispatcher(), this);
    listen(core.getPlayers().getPlayerSpawnDispatcher(), this);
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

void PlayerStateSystem::onPlayerStateChange(IPlayer &player, PlayerState newState, PlayerState oldState)
{
    const TimePoint now = std::chrono::steady_clock::now();
    PlayerStateService::StateOutcome outcome = m_stateService.onStateChange(player, newState, oldState, now);
    if (outcome.stateHack)
    {
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::StateHack, std::move(outcome.detail),
                                  now);
    }
}

bool PlayerStateSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    PlayerStateService::ActionOutcome outcome = m_stateService.verifyAction(player, now);
    if (outcome.actionHack)
    {
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::SpecialActionHack,
                                  std::move(outcome.detail), now);
    }
    else if (outcome.enforceEject)
    {
        // Рычаг, который клиент отменить не может: машину респавнит СЕРВЕР, после
        // чего ядро перестаёт принимать driver-sync для неё — игрок исчезает из
        // машины для всех, независимо от того, что рисует его клиент. Сам RPC
        // высадки такой гарантии не даёт (это тоже пакет, его можно игнорировать).
        if (IVehicle *vehicle = m_vehicleService.get(outcome.ejectVehicleId))
        {
            m_vehicleService.respawn(*vehicle);
        }
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::VehicleEjectEvasion,
                                  std::move(outcome.detail), now);
    }
    return true;
}

void PlayerStateSystem::onPlayerSpawn(IPlayer &player)
{
    m_stateService.onSpawn(player);
}

void PlayerStateSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_stateService.reset(player.getID());
}
