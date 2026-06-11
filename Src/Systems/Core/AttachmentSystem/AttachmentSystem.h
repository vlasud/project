#pragma once

#include "Services/Core/AttachmentService/AttachmentService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник AttachmentService: маршрутизирует событие клиентской подгонки
// прикреплённого объекта (того же диспатчера объектов, что и ObjectEditSystem —
// подписки по событиям не пересекаются) и чистит состояние при выходе.
class AttachmentSystem : public BaseSystem, public ObjectEventHandler, public PlayerConnectEventHandler
{
  public:
    AttachmentSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerAttachedObjectEdited(IPlayer &player, int index, bool saved,
                                      const ObjectAttachmentSlotData &data) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    AttachmentService &m_attachmentService;
};
