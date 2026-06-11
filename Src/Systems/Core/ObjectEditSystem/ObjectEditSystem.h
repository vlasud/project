#pragma once

#include "Services/Core/ObjectEditService/ObjectEditService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Единственный подписчик событий объектов: маршрутизирует нативное
// редактирование/выбор в ObjectEditService и чистит сессии при выходе.
class ObjectEditSystem : public BaseSystem, public ObjectEventHandler, public PlayerConnectEventHandler
{
  public:
    ObjectEditSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onObjectEdited(IPlayer &player, IObject &object, ObjectEditResponse response, Vector3 offset,
                        Vector3 rotation) override;
    void onObjectSelected(IPlayer &player, IObject &object, int model, Vector3 position) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    ObjectEditService &m_objectEditService;
};
