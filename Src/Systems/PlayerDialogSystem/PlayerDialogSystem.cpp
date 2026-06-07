#include "PlayerDialogSystem.h"
#include "Server/Components/Dialogs/dialogs.hpp"

PlayerDialogSystem::PlayerDialogSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_playerDialogService(serviceRegister.getService<PlayerDialogService>())
{
}

void PlayerDialogSystem::initialize(IComponentList *components)
{
    IDialogsComponent *dialogComponent = components->queryComponent<IDialogsComponent>();
    dialogComponent->getEventDispatcher().addEventHandler(this);
}

void PlayerDialogSystem::onDialogResponse(IPlayer &player, int dialogId, DialogResponse response, int listItem,
                                          StringView inputText)
{
    const int playerId = player.getID();

    if (!m_playerDialogService.validateDialog(playerId, dialogId))
    {
        return;
    }

    Dialog &dialog = m_playerDialogService.getPlayerDialog(playerId);

    if (response == DialogResponse_Right && dialog.rightAction)
    {
        dialog.rightAction(playerId, listItem, inputText);
    }
    else if (response == DialogResponse_Left && dialog.leftAction)
    {
        dialog.leftAction(playerId, listItem, inputText);
    }
}
