#pragma once

#include "Services/Core/GridService/GridService.h"
#include "Services/Core/PlayerChatService/PlayerChatService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include "types.hpp"
#include <vector>

// Ролевые проксимити-эмоуты: /me, /do, /try слышат игроки в радиусе RP_RADIUS
// того же виртуального мира (поиск через сетку). Имя берётся серверно, текст —
// сырой клиентский cp1251. Мут и антиспам — через PlayerChatService (тот же
// барьер, что у обычного чата: через эмоуты мут не обойти).
class RoleplayChatSystem : public BaseSystem
{
  public:
    RoleplayChatSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Мут/антиспам через PlayerChatService: при блокировке шлёт игроку причину и
    // возвращает false (эмоут не рассылается). text — cp1251, как у обычного чата.
    bool passChatGate(IPlayer &player, StringView text);

    // Рассылка готовой строки всем ближним того же виртуального мира.
    void broadcast(IPlayer &author, StringView line);

    GridService &m_gridService;
    PlayerChatService &m_chatService;
    PlayerLocationService &m_locationService;

    std::vector<GridService::Result> m_listeners; // переиспользуемый буфер запроса
};
