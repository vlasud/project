#pragma once

#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Systems/BaseSystem.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "player.hpp"

// Единственный подписчик на onDialogResponse: маршрутизирует ответы в PlayerDialogService
// и чистит per-player слот диалога при отключении игрока.
class PlayerDialogSystem : public BaseSystem, public PlayerDialogEventHandler, public PlayerConnectEventHandler
{
  public:
    PlayerDialogSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onDialogResponse(IPlayer &player, int dialogId, DialogResponse response, int listItem,
                          StringView inputText) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerDialogService &m_playerDialogService;
};
