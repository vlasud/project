#include "Systems/Core/AttachmentSystem/AttachmentSystem.h"

#include "Log/LogManager.h"

AttachmentSystem::AttachmentSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_attachmentService(serviceRegister.getService<AttachmentService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

void AttachmentSystem::initialize(IComponentList *components)
{
    IObjectsComponent *objects = components->queryComponent<IObjectsComponent>();
    if (!objects)
    {
        LogManager::log(Error, "AttachmentSystem: IObjectsComponent is missing, attachments editing is disabled");
        return;
    }
    listen(objects->getEventDispatcher(), this);
}

void AttachmentSystem::onPlayerAttachedObjectEdited(IPlayer &player, int index, bool saved,
                                                    const ObjectAttachmentSlotData &data)
{
    m_attachmentService.handleEdited(player, index, saved, data);
}

void AttachmentSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_attachmentService.resetPlayer(player.getID());
}
