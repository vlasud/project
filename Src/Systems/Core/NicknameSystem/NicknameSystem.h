#pragma once

#include "Services/Core/NicknameService/NicknameService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Отсев невалидных ников по политике NicknameService. Проверка на
// onPlayerConnect (раньше, на входящем подключении, клиент ещё не
// инициализирован и диалог не отрисует): игроку показывается диалог с
// правилами, кик — по закрытию диалога или по страховочному таймеру, если
// диалог проигнорирован.
class NicknameSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    NicknameSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;

  private:
    void reject(IPlayer &player, const std::string &reason);

    NicknameService &m_nicknameService;
    PlayerDialogService &m_dialogService;
    TimerService &m_timerService;
};
