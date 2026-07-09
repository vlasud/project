#include "Services/Core/MovingObjectService/MovingObjectService.h"

#include "Log/LogManager.h"
#include "Utils/Sanitize.h"
#include <algorithm>
#include <cmath>

namespace
{
constexpr int MAX_OBJECT_MODEL = 19999;
} // namespace

int MovingObjectService::create(int model, const Vector3 &position, const Vector3 &rotation, float drawDistance)
{
    if (!m_objects)
    {
        LogManager::log(Error, "MovingObjectService: IObjectsComponent is missing");
        return -1;
    }

    model = std::clamp(model, 0, MAX_OBJECT_MODEL);
    IObject *object = m_objects->create(model, Utils::sanitize(position), Utils::sanitize(rotation),
                                        std::isfinite(drawDistance) ? drawDistance : 0.0f);
    if (!object)
    {
        LogManager::log(Warning, "MovingObjectService: global object pool is full");
        return -1;
    }

    m_defs[object->getID()] = Def{};
    return object->getID();
}

void MovingObjectService::destroy(int objectId)
{
    if (m_defs.erase(objectId) > 0 && m_objects)
    {
        m_objects->release(objectId);
    }
}

bool MovingObjectService::exists(int objectId) const
{
    return m_defs.find(objectId) != m_defs.end();
}

bool MovingObjectService::moveTo(int objectId, const Vector3 &targetPosition, const Vector3 &targetRotation,
                                 float speed, ArriveHandler onArrived)
{
    IObject *object = getManaged(objectId);
    if (!object)
    {
        return false;
    }

    // Новый moveTo заменяет предыдущий вместе с его обработчиком прибытия.
    m_defs[objectId].onArrived = std::move(onArrived);

    ObjectMoveData data;
    data.targetPos = Utils::sanitize(targetPosition);
    data.targetRot = Utils::sanitize(targetRotation);
    data.speed = std::clamp(std::isfinite(speed) ? speed : MIN_SPEED, MIN_SPEED, MAX_SPEED);
    object->move(data);
    return true;
}

void MovingObjectService::stopMoving(int objectId)
{
    IObject *object = getManaged(objectId);
    if (!object)
    {
        return;
    }

    // Обработчик снимаем до stop(): если ядро на остановке диспатчит onMoved,
    // прибытием это не считается.
    m_defs[objectId].onArrived = nullptr;
    if (object->isMoving())
    {
        object->stop();
    }
}

bool MovingObjectService::isMoving(int objectId) const
{
    IObject *object = getManaged(objectId);
    return object && object->isMoving();
}

void MovingObjectService::setTransform(int objectId, const Vector3 &position, const Vector3 &rotation)
{
    IObject *object = getManaged(objectId);
    if (!object)
    {
        return;
    }

    m_defs[objectId].onArrived = nullptr; // телепорт прерывает движение без прибытия
    if (object->isMoving())
    {
        object->stop();
    }
    object->setPosition(Utils::sanitize(position));
    object->setRotation(GTAQuat(Utils::sanitize(rotation)));
}

Vector3 MovingObjectService::getPosition(int objectId) const
{
    IObject *object = getManaged(objectId);
    return object ? object->getPosition() : Vector3(0.0f, 0.0f, 0.0f);
}

// ------------------------------------------------------------------ вызовы MovingObjectSystem

void MovingObjectService::initialize(IObjectsComponent *objects)
{
    m_objects = objects;
}

void MovingObjectService::handleMoved(IObject &object)
{
    auto it = m_defs.find(object.getID());
    if (it == m_defs.end())
    {
        return; // чужой объект (редактор и т.п.)
    }

    // Копия до вызова: обработчик может тут же запустить обратное движение
    // (дверь закрывается) или уничтожить объект.
    ArriveHandler handler = std::move(it->second.onArrived);
    it->second.onArrived = nullptr;
    if (handler)
    {
        handler(object);
    }
}

IObject *MovingObjectService::getManaged(int objectId) const
{
    if (!m_objects || m_defs.find(objectId) == m_defs.end())
    {
        return nullptr;
    }
    return m_objects->get(objectId);
}
