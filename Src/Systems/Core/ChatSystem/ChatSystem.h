#pragma once

#include "Services/Core/GridService/GridService.h"
#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"
#include "Services/Core/PlayerChatService/PlayerChatService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <vector>

// Проксимити-чат: сообщение слышат игроки в радиусе (поиск через сетку), цвет
// тускнеет с расстоянием (ближе — белее). Говорящий проигрывает прерываемую
// анимацию разговора. Мут и антиспам — через PlayerChatService.
// Команды /mute [id] [секунды] и /unmute [id] (до появления админ-системы).
class ChatSystemSystem : public BaseSystem, public PlayerTextEventHandler, public PlayerConnectEventHandler
{
  public:
    ChatSystemSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerText(IPlayer &player, StringView message) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    GridService &m_gridService;
    PlayerChatService &m_chatService;
    PlayerLocationService &m_locationService;
    PlayerAnimationService &m_animationService;
    PlayerStateService &m_stateService;

    std::vector<GridService::Result> m_listeners; // переиспользуемый буфер запроса
};
