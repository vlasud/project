#include "Systems/Core/PlayerAnimationSystem/PlayerAnimationSystem.h"

PlayerAnimationSystem::PlayerAnimationSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_animationService(serviceRegister.getService<PlayerAnimationService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>())
{
    listen(core.getPlayers().getPlayerUpdateDispatcher(), this);
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

bool PlayerAnimationSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    PlayerAnimationService::VerifyOutcome outcome = m_animationService.verify(player, now);
    if (outcome.forcedAnimationEscaped)
    {
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::ForcedAnimationEscape,
                                  std::move(outcome.detail), now);
    }
    return true;
}

void PlayerAnimationSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_animationService.reset(player.getID());
    // Журнал нарушений чистит AntiCheatSystem — она владеет его жизненным циклом.
}
