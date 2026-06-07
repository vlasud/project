#pragma once

#include "../../Services/PlayerDialogService/PlayerDialogService.h"
#include "../BaseSystem.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "player.hpp"

class PlayerDialogSystem : public BaseSystem, public PlayerDialogEventHandler
{
  public:
    PlayerDialogSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onDialogResponse(IPlayer &player, int dialogId, DialogResponse response, int listItem,
                          StringView inputText) override;

  private:
    PlayerDialogService &m_playerDialogService;
};
