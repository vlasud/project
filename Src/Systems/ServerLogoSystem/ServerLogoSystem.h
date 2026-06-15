#pragma once

#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Логотип сервера «HARDWAY» — глобальный textdraw, один на всех. Создаётся один
// раз при готовом компоненте textdraw и живёт всю сессию сервера; каждому
// игроку показывается на подключении. Своего источника состояния нет, только
// id созданного логотипа.
class ServerLogoSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    ServerLogoSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerConnect(IPlayer &player) override;

  private:
    TextDrawService &m_textDrawService;
    int m_logoId = -1; // id глобального textdraw; -1 — логотип не создан
};
