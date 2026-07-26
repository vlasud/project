#include "Systems/Core/MovingObjectSystem/MovingObjectSystem.h"

#include "Log/LogManager.h"

MovingObjectSystem::MovingObjectSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_movingObjectService(serviceRegister.getService<MovingObjectService>())
{
}

void MovingObjectSystem::initialize(IComponentList *components)
{
    IObjectsComponent *objects = components->queryComponent<IObjectsComponent>();
    if (!objects)
    {
        LogManager::log(Error, "MovingObjectSystem: IObjectsComponent is missing, moving objects are disabled");
        return;
    }
    m_movingObjectService.initialize(objects);
    listen(objects->getEventDispatcher(), this);
}

void MovingObjectSystem::onMoved(IObject &object)
{
    m_movingObjectService.handleMoved(object);
}
