#pragma once

#include "Services/Core/MovingObjectService/MovingObjectService.h"
#include "Systems/BaseSystem.h"

// Проводник MovingObjectService: маршрутизирует событие прибытия объекта
// (onMoved считает ядро, двигающее объект) в сервис. События редактирования
// того же диспатчера обрабатывает ObjectEditSystem — подписки не пересекаются.
class MovingObjectSystem : public BaseSystem, public ObjectEventHandler
{
  public:
    MovingObjectSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onMoved(IObject &object) override;

  private:
    MovingObjectService &m_movingObjectService;
};
