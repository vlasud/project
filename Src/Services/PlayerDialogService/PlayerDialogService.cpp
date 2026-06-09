#include "PlayerDialogService.h"
#include "../../Log/LogManager.h"
#include "component.hpp"

void PlayerDialogService::show(IPlayer &player, const Dialog &dialog, Handler handler)
{
    IPlayerDialogData *data = queryExtension<IPlayerDialogData>(player);
    if (!data)
    {
        LogManager::log(Error, "PlayerDialogService: IPlayerDialogData extension is missing");
        return;
    }

    Slot &slot = m_slots[player.getID()];
    slot.serial = slot.serial % 32000 + 1; // id в диапазоне 1..32000, новый на каждый показ
    slot.activeId = slot.serial;
    slot.handler = std::move(handler);

    data->show(player, slot.activeId, dialog.style, dialog.title, dialog.body, dialog.leftButton, dialog.rightButton);
}

void PlayerDialogService::hide(IPlayer &player)
{
    Slot &slot = m_slots[player.getID()];
    slot.activeId = -1;
    slot.handler = nullptr;

    if (IPlayerDialogData *data = queryExtension<IPlayerDialogData>(player))
    {
        data->hide(player);
    }
}

void PlayerDialogService::handleResponse(IPlayer &player, int dialogId, DialogResponse response, int listItem,
                                         StringView inputText)
{
    Slot &slot = m_slots[player.getID()];
    if (dialogId != slot.activeId || !slot.handler)
    {
        return; // запоздалый или подделанный ответ
    }

    // Забираем колбэк до вызова: внутри него обычно show() следующего экрана, который займёт слот.
    Handler handler = std::move(slot.handler);
    slot.handler = nullptr;
    slot.activeId = -1;

    handler(response, listItem, inputText);
}

void PlayerDialogService::resetPlayer(int playerId)
{
    // serial не сбрасываем: он продолжает защищать от запоздалых ответов прошлого подключения.
    m_slots[playerId].activeId = -1;
    m_slots[playerId].handler = nullptr;
}
