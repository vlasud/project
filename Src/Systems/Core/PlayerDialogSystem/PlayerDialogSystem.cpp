#include "Systems/Core/PlayerDialogSystem/PlayerDialogSystem.h"
#include "Log/LogManager.h"
#include "Server/Components/Dialogs/dialogs.hpp"

PlayerDialogSystem::PlayerDialogSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_playerDialogService(serviceRegister.getService<PlayerDialogService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

void PlayerDialogSystem::initialize(IComponentList *components)
{
    IDialogsComponent *dialogComponent = components->queryComponent<IDialogsComponent>();
    if (!dialogComponent)
    {
        LogManager::log(Error, "PlayerDialogSystem: IDialogsComponent is missing, dialogs are disabled");
        return;
    }
    listen(dialogComponent->getEventDispatcher(), this);
}

void PlayerDialogSystem::onDialogResponse(IPlayer &player, int dialogId, DialogResponse response, int listItem,
                                          StringView inputText)
{
    m_playerDialogService.handleResponse(player, dialogId, response, listItem, inputText);
}

void PlayerDialogSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_playerDialogService.resetPlayer(player.getID());
}
