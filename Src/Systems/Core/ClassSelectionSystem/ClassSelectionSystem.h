#pragma once

#include "Services/Core/ClassSelectionService/ClassSelectionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Classes/classes.hpp>

// Единственный подписчик событий класс-селекшна: маршрутизирует запросы класса
// и клиентского спавна в ClassSelectionService, сообщает ему о смертях (для
// валидации легальности входа/респауна) и сбрасывает состояние на спавне/выходе.
class ClassSelectionSystem : public BaseSystem,
                             public ClassEventHandler,
                             public PlayerSpawnEventHandler,
                             public PlayerDamageEventHandler,
                             public PlayerConnectEventHandler
{
  public:
    ClassSelectionSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    bool onPlayerRequestClass(IPlayer &player, unsigned int classId) override;
    bool onPlayerRequestSpawn(IPlayer &player) override;
    void onPlayerDeath(IPlayer &player, IPlayer *killer, int reason) override;
    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    ClassSelectionService &m_classSelectionService;
};
