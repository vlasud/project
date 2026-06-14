#pragma once

#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

class PlayerCommandSystem : public BaseSystem, public PlayerTextEventHandler, public PlayerConnectEventHandler
{
  public:
    PlayerCommandSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerCommandText(IPlayer &player, StringView message) override;
    // Сброс антифлуд-состояния слота, иначе чужой блок/счётчик утечёт в
    // переиспользованный id.
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerCommandService &m_commandService;
    // Владелец сервиса команд: здесь задаётся общий резолвер прав, транслирующий
    // PermissionSpec в факты этих сервисов. Все сервисы конструируются до систем,
    // поэтому ссылки валидны на всё время жизни.
    AdminService &m_adminService;
    FactionService &m_factionService;
};
