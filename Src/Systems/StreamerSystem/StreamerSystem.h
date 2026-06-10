#pragma once

#include "../../Services/GridService/GridService.h"
#include "../../Services/StreamerService/StreamerService.h"
#include "../BaseSystem.h"
#include "player.hpp"

// Привод стримера: проход видимости игрока на его апдейтах (троттлинг внутри
// сервиса), глобальная развёртка пикапов на тике ядра, сброс учёта на дисконнекте.
class StreamerSystem : public BaseSystem,
                       public PlayerUpdateEventHandler,
                       public PlayerConnectEventHandler,
                       public CoreEventHandler
{
  public:
    StreamerSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;
    void onTick(Microseconds elapsed, TimePoint now) override;

  private:
    GridService &m_gridService;
    StreamerService &m_streamerService;

    TimePoint m_nextPickupSweep{};
};
