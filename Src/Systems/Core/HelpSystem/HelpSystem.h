#pragma once

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// /help — справка игрока: диалог со списком доступных ему геймплейных команд,
// сгруппированных по категориям. Парный близнец /ahelp (тот же вид TABLIST и
// тон описаний), но другой охват: показываются команды с заявленной категорией
// (не Hidden), плюс секция «Фракция» — только члену организации.
//
// Охват берётся из РЕЕСТРА команд (единый источник истины: описание+категория
// при add()), а не из PermissionSpec: фракционные команды зарегистрированы как
// Kind::None (право проверяется внутри обработчика), поэтому фракционность
// определяется категорией + фактом членства (getMemberFaction).
class HelpSystem : public BaseSystem
{
  public:
    HelpSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Диалог со списком доступных игроку команд (по категориям + членство).
    void cmdHelp(IPlayer &player);

    PlayerCommandService &m_commandService;
    FactionService &m_factionService;
    PlayerDialogService &m_dialogService;
};
