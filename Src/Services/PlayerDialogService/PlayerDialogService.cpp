#include "PlayerDialogService.h"
#include "../../Log/LogManager.h"
#include "component.hpp"

int PlayerDialogService::buildDialog(Dialog &&dialog)
{
    m_dialogs.emplace_back(std::move(dialog));
    return m_dialogs.size() - 1;
}

void PlayerDialogService::showDialog(IPlayer &player, int dialogId)
{
    if (!m_dialogExtension)
    {
        m_dialogExtension = queryExtension<IPlayerDialogData>(player);
    }

    if (!m_dialogExtension)
    {
        LogManager::log(Message, "Failed to load IPlayerDialogData");
        assert(false && "Failed to load IPlayerDialogData");
        return;
    }

    if (dialogId >= m_dialogs.size())
    {
        dialogId = -1;
    }

    m_playersCurrentDialogId[player.getID()] = dialogId;

    if (dialogId < 0)
    {
        return;
    }

    const Dialog &dialog = m_dialogs[dialogId];
    m_dialogExtension->show(player, dialogId, dialog.style, dialog.title, dialog.body, dialog.leftButton,
                            dialog.rightButton);
}

void PlayerDialogService::setDialogBody(int dialogId, StringView body)
{
    Dialog &dialog = m_dialogs[dialogId];
    dialog.body = body.data();
}

bool PlayerDialogService::validateDialog(int playerId, int dialogId)
{
    return m_playersCurrentDialogId[playerId] == dialogId;
}

Dialog &PlayerDialogService::getPlayerDialog(int playerId)
{
    return m_dialogs[m_playersCurrentDialogId[playerId]];
}
