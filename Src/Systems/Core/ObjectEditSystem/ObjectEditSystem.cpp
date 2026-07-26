#include "Systems/Core/ObjectEditSystem/ObjectEditSystem.h"

#include "Log/LogManager.h"

ObjectEditSystem::ObjectEditSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_objectEditService(serviceRegister.getService<ObjectEditService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

void ObjectEditSystem::initialize(IComponentList *components)
{
    IObjectsComponent *objects = components->queryComponent<IObjectsComponent>();
    if (!objects)
    {
        LogManager::log(Error, "ObjectEditSystem: IObjectsComponent is missing, object editing is disabled");
        return;
    }
    listen(objects->getEventDispatcher(), this);
}

void ObjectEditSystem::onObjectEdited(IPlayer &player, IObject &object, ObjectEditResponse response, Vector3 offset,
                                      Vector3 rotation)
{
    m_objectEditService.handleEdited(player, object, response, offset, rotation);
}

void ObjectEditSystem::onObjectSelected(IPlayer &player, IObject &object, int model, Vector3 position)
{
    m_objectEditService.handleSelected(player, object, model, position);
}

void ObjectEditSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_objectEditService.resetPlayer(player.getID());
}
