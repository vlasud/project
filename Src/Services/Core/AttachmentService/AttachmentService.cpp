#include "Services/Core/AttachmentService/AttachmentService.h"

#include "Log/LogManager.h"
#include <algorithm>
#include <cmath>

namespace
{
constexpr int MAX_MODEL = 19999;
// Пределы подгонки: оффсет дальше — объект болтается в стороне от тела,
// масштаб больше — закрывает экран окружающим (и то и другое видно всем).
constexpr float MAX_OFFSET = 10.0f;
constexpr float MIN_SCALE = 0.01f;
constexpr float MAX_SCALE = 10.0f;

float finiteOr(float value, float fallback)
{
    return std::isfinite(value) ? value : fallback;
}

ObjectAttachmentSlotData makeData(int model, PlayerBone bone, const Vector3 &offset, const Vector3 &rotation,
                                  const Vector3 &scale, Colour colour1, Colour colour2)
{
    ObjectAttachmentSlotData data;
    data.model = model;
    data.bone = static_cast<int>(bone);
    data.offset = offset;
    data.rotation = rotation;
    data.scale = scale;
    data.colour1 = colour1;
    data.colour2 = colour2;
    return data;
}
} // namespace

bool AttachmentService::sanitize(ObjectAttachmentSlotData &data)
{
    if (data.model < 0 || data.model > MAX_MODEL)
    {
        return false;
    }
    if (data.bone <= PlayerBone_None || data.bone > PlayerBone_Jaw)
    {
        return false;
    }

    auto clampOffset = [](float v) { return std::clamp(finiteOr(v, 0.0f), -MAX_OFFSET, MAX_OFFSET); };
    auto clampScale = [](float v) { return std::clamp(finiteOr(v, 1.0f), MIN_SCALE, MAX_SCALE); };
    auto wrapAngle = [](float v) { return finiteOr(v, 0.0f); };

    data.offset = {clampOffset(data.offset.x), clampOffset(data.offset.y), clampOffset(data.offset.z)};
    data.rotation = {wrapAngle(data.rotation.x), wrapAngle(data.rotation.y), wrapAngle(data.rotation.z)};
    data.scale = {clampScale(data.scale.x), clampScale(data.scale.y), clampScale(data.scale.z)};
    return true;
}

int AttachmentService::attach(IPlayer &player, int model, PlayerBone bone, const Vector3 &offset,
                              const Vector3 &rotation, const Vector3 &scale, Colour colour1, Colour colour2)
{
    const State &st = m_state[player.getID()];
    for (int slot = 0; slot < MAX_SLOTS; ++slot)
    {
        if (!st.used[slot])
        {
            return attachToSlot(player, slot, model, bone, offset, rotation, scale, colour1, colour2) ? slot : -1;
        }
    }
    LogManager::log(Warning,
                    "AttachmentService: no free attachment slots for player " + std::to_string(player.getID()));
    return -1;
}

bool AttachmentService::attachToSlot(IPlayer &player, int slot, int model, PlayerBone bone, const Vector3 &offset,
                                     const Vector3 &rotation, const Vector3 &scale, Colour colour1, Colour colour2)
{
    if (slot < 0 || slot >= MAX_SLOTS)
    {
        return false;
    }
    IPlayerObjectData *objects = queryExtension<IPlayerObjectData>(player);
    if (!objects)
    {
        LogManager::log(Error, "AttachmentService: IPlayerObjectData extension is missing");
        return false;
    }

    ObjectAttachmentSlotData data = makeData(model, bone, offset, rotation, scale, colour1, colour2);
    if (!sanitize(data))
    {
        return false;
    }

    State &st = m_state[player.getID()];
    st.used[slot] = true;
    st.data[slot] = data;
    objects->setAttachedObject(slot, data);
    return true;
}

void AttachmentService::detach(IPlayer &player, int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS)
    {
        return;
    }
    State &st = m_state[player.getID()];
    if (!st.used[slot])
    {
        return;
    }

    st.used[slot] = false;
    if (st.editingSlot == slot)
    {
        st.editingSlot = -1;
        st.onEdit = nullptr;
    }
    if (IPlayerObjectData *objects = queryExtension<IPlayerObjectData>(player))
    {
        objects->removeAttachedObject(slot);
    }
}

void AttachmentService::detachAll(IPlayer &player)
{
    for (int slot = 0; slot < MAX_SLOTS; ++slot)
    {
        detach(player, slot);
    }
}

bool AttachmentService::isAttached(int playerId, int slot) const
{
    return slot >= 0 && slot < MAX_SLOTS && m_state[playerId].used[slot];
}

bool AttachmentService::beginEdit(IPlayer &player, int slot, EditHandler onDone)
{
    State &st = m_state[player.getID()];
    if (slot < 0 || slot >= MAX_SLOTS || !st.used[slot])
    {
        return false;
    }
    IPlayerObjectData *objects = queryExtension<IPlayerObjectData>(player);
    if (!objects)
    {
        return false;
    }

    st.editingSlot = slot;
    st.onEdit = std::move(onDone);
    objects->editAttachedObject(slot);
    return true;
}

// ------------------------------------------------------------------ вызовы AttachmentSystem

void AttachmentService::handleEdited(IPlayer &player, int slot, bool saved, const ObjectAttachmentSlotData &data)
{
    State &st = m_state[player.getID()];

    // Применяем только подгонку, которую начал СЕРВЕР и ровно для того слота:
    // клиент в чужой edit-сессии может прислать событие для любого своего слота.
    if (st.editingSlot != slot || slot < 0 || slot >= MAX_SLOTS || !st.used[slot])
    {
        return;
    }
    st.editingSlot = -1;

    IPlayerObjectData *objects = queryExtension<IPlayerObjectData>(player);
    if (saved)
    {
        // Оффсеты/масштаб пришли от клиента — санитизируем до применения
        // (гигантский масштаб видят все вокруг). Модель/кость ядро уже сверило.
        ObjectAttachmentSlotData sanitized = data;
        if (sanitize(sanitized))
        {
            st.data[slot] = sanitized;
            if (objects)
            {
                objects->setAttachedObject(slot, sanitized); // переприменяем уже чистые данные
            }
        }
        else
        {
            saved = false; // мусор — откат на прежний вид
        }
    }
    if (!saved && objects)
    {
        objects->setAttachedObject(slot, st.data[slot]); // откат (ESC или мусор)
    }

    // Копия: обработчик может начать новую подгонку или снять слот.
    EditHandler handler = std::move(st.onEdit);
    st.onEdit = nullptr;
    if (handler)
    {
        handler(player, slot, saved);
    }
}

void AttachmentService::resetPlayer(int playerId)
{
    // Клиентское состояние умерло вместе с подключением — чистим только учёт.
    m_state[playerId] = State{};
}
