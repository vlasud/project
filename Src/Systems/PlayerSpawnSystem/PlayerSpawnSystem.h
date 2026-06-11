#pragma once

#include "Services/PlayerSpawnService/PlayerSpawnService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник PlayerSpawnService: прокидывает точку спавна в class-данные при
// подключении, применяет интерьер/мир на событии спавна и чистит состояние
// при выходе. Регистрируется раньше бизнес-систем, навешивающих экипировку на
// спавн, — к их ходу игрок уже в правильном месте и мире.
class PlayerSpawnSystem : public BaseSystem, public PlayerConnectEventHandler, public PlayerSpawnEventHandler
{
  public:
    PlayerSpawnSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerSpawnService &m_spawnService;
};
