#include "Systems/Core/AudioSystem/AudioSystem.h"

AudioSystem::AudioSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_audioService(serviceRegister.getService<AudioService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);

    m_audioService.initialize(&core);
}

void AudioSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_audioService.resetPlayer(player.getID());
}
