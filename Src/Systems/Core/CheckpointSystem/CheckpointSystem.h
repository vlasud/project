#pragma once

#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Единственный подписчик на события чекпоинтов: маршрутизирует enter/leave в
// CheckpointService (там валидация и обработчики), гоняет стриминг глобальных
// чекпоинтов по принятой позиции и чистит per-player состояние при выходе.
class CheckpointSystem : public BaseSystem,
                         public PlayerCheckpointEventHandler,
                         public PlayerUpdateEventHandler,
                         public PlayerConnectEventHandler
{
  public:
    CheckpointSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerEnterCheckpoint(IPlayer &player) override;
    void onPlayerLeaveCheckpoint(IPlayer &player) override;
    void onPlayerEnterRaceCheckpoint(IPlayer &player) override;
    void onPlayerLeaveRaceCheckpoint(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    CheckpointService &m_checkpointService;
    PlayerLocationService &m_locationService;
};
