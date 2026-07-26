#include "Services/Core/ObjectEditService/ObjectEditService.h"

#include "Log/LogManager.h"

bool ObjectEditService::beginEdit(IPlayer &player, IObject &object, EditHandler handler)
{
    IPlayerObjectData *data = queryExtension<IPlayerObjectData>(player);
    if (!data)
    {
        LogManager::log(Error, "ObjectEditService: IPlayerObjectData extension is missing");
        return false;
    }

    Slot &slot = m_slots[player.getID()];
    slot.editObjectId = object.getID();
    slot.editHandler = std::move(handler);
    slot.selectHandler = nullptr;

    data->beginEditing(object);
    return true;
}

bool ObjectEditService::beginSelect(IPlayer &player, SelectHandler handler)
{
    IPlayerObjectData *data = queryExtension<IPlayerObjectData>(player);
    if (!data)
    {
        LogManager::log(Error, "ObjectEditService: IPlayerObjectData extension is missing");
        return false;
    }

    Slot &slot = m_slots[player.getID()];
    slot.editObjectId = -1;
    slot.editHandler = nullptr;
    slot.selectHandler = std::move(handler);

    data->beginSelecting();
    return true;
}

void ObjectEditService::end(IPlayer &player)
{
    resetPlayer(player.getID());
    if (IPlayerObjectData *data = queryExtension<IPlayerObjectData>(player))
    {
        data->endEditing();
    }
}

bool ObjectEditService::isEditing(int playerId) const
{
    if (!validPlayerId(playerId))
        return false;
    return m_slots[playerId].editObjectId >= 0;
}

// ------------------------------------------------------------------ вызовы ObjectEditSystem

void ObjectEditService::handleEdited(IPlayer &player, IObject &object, ObjectEditResponse response, Vector3 position,
                                     Vector3 rotation)
{
    Slot &slot = m_slots[player.getID()];

    // Ядро проверяет лишь «игрок в сессии редактирования», но не какой объект:
    // правка с чужим ObjectID — подделка, игнорируем.
    if (slot.editObjectId != object.getID() || !slot.editHandler)
    {
        return;
    }

    // Копия: на Final/Cancel сессия закрывается до вызова, обработчик может
    // сразу начать новую.
    EditHandler handler = slot.editHandler;
    if (response == ObjectEditResponse_Final || response == ObjectEditResponse_Cancel)
    {
        slot.editObjectId = -1;
        slot.editHandler = nullptr;
    }

    handler(player, object, response, position, rotation);
}

void ObjectEditService::handleSelected(IPlayer &player, IObject &object, int model, Vector3 position)
{
    Slot &slot = m_slots[player.getID()];
    if (!slot.selectHandler)
    {
        return; // сервер выбор не запускал
    }

    // Одноразовый: снимаем до вызова — обработчик может начать редактирование.
    SelectHandler handler = std::move(slot.selectHandler);
    slot.selectHandler = nullptr;
    handler(player, object, model, position);
}

void ObjectEditService::resetPlayer(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    m_slots[playerId] = Slot{};
}
