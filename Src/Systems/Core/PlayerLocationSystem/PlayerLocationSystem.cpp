#include "Systems/Core/PlayerLocationSystem/PlayerLocationSystem.h"

#include <chrono>

PlayerLocationSystem::PlayerLocationSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>())
{
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerChangeDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

bool PlayerLocationSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    PlayerLocationService::VerifyOutcome outcome = m_locationService.verify(player, now);
    if (outcome.teleportHack)
    {
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::TeleportHack,
                                  std::move(outcome.detail), now);
    }
    return true;
}

void PlayerLocationSystem::onPlayerSpawn(IPlayer &player)
{
    m_locationService.onSpawn(player);
}

void PlayerLocationSystem::onPlayerInteriorChange(IPlayer &player, unsigned newInterior, unsigned oldInterior)
{
    m_locationService.onInteriorChange(player, newInterior, std::chrono::steady_clock::now());
}

void PlayerLocationSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_locationService.reset(player.getID());
}
