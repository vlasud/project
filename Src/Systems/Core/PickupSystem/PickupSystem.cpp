#include "Systems/Core/PickupSystem/PickupSystem.h"

#include "Log/LogManager.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/StreamerService/StreamerService.h"

PickupSystem::PickupSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_pickupService(serviceRegister.getService<PickupService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);

    m_pickupService.initialize(&serviceRegister.getService<StreamerService>(),
                               &serviceRegister.getService<PlayerLocationService>(),
                               &serviceRegister.getService<AntiCheatService>());
}

void PickupSystem::initialize(IComponentList *components)
{
    IPickupsComponent *pickups = components->queryComponent<IPickupsComponent>();
    if (!pickups)
    {
        LogManager::log(Error, "PickupSystem: IPickupsComponent is missing, pickups are disabled");
        return;
    }
    listen(pickups->getEventDispatcher(), this);
}

void PickupSystem::onPlayerPickUpPickup(IPlayer &player, IPickup &pickup)
{
    m_pickupService.handlePickUp(player, pickup, Time::now());
}

void PickupSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_pickupService.resetPlayer(player.getID());
}
