#include "PlayerVelocitySystem.h"

#include "../../Services/PlayerLocationService/PlayerLocationService.h"

PlayerVelocitySystem::PlayerVelocitySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_velocityService(serviceRegister.getService<PlayerVelocityService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>())
{
    m_velocityService.bind(serviceRegister.getService<PlayerLocationService>());

    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

bool PlayerVelocitySystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    PlayerVelocityService::VerifyOutcome outcome = m_velocityService.sample(player, now);
    if (outcome.speedHack)
    {
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::SpeedHack,
                                  std::move(outcome.detail), now);
    }
    return true;
}

void PlayerVelocitySystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_velocityService.reset(player.getID());
}
