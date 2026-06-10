#include "PlayerAnimationSystem.h"

PlayerAnimationSystem::PlayerAnimationSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_animationService(serviceRegister.getService<PlayerAnimationService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>())
{
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
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

    // Журнал нарушений — пер-слотовый, поэтому чистим при отключении, чтобы новый
    // игрок в том же слоте не унаследовал чужие записи. Когда появится отдельная
    // система-античит, управление жизненным циклом записей логично перенести в неё.
    m_antiCheatService.clear(player.getID());
}
