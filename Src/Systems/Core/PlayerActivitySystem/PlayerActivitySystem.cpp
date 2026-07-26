#include "Systems/Core/PlayerActivitySystem/PlayerActivitySystem.h"

#include "Log/LogManager.h"
#include <chrono>

PlayerActivitySystem::PlayerActivitySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_activityService(serviceRegister.getService<PlayerActivityService>()),
      m_timerService(serviceRegister.getService<TimerService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
    listen(core.getPlayers().getPlayerUpdateDispatcher(), this);
}

void PlayerActivitySystem::initialize(IComponentList *components)
{
    ITextLabelsComponent *labels = components->queryComponent<ITextLabelsComponent>();
    if (!labels)
    {
        LogManager::log(Error, "PlayerActivitySystem: ITextLabelsComponent is missing, pause labels are disabled");
    }
    m_activityService.initialize(&m_core, labels);

    // Паузнутые игроки апдейтов не шлют — детект гоняется таймером.
    m_timerService.setInterval(std::chrono::seconds(1), [this] { m_activityService.sweep(Time::now()); });
}

bool PlayerActivitySystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    m_activityService.handleUpdate(player, now);
    return true;
}

void PlayerActivitySystem::onPlayerConnect(IPlayer &player)
{
    m_activityService.handleConnect(player, Time::now());
}

void PlayerActivitySystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_activityService.resetPlayer(player.getID());
}
