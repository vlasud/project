#pragma once

#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerChatService/PlayerChatService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/ReportService/ReportService.h"
#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// /mn — единая стартовая панель для ВСЕХ игроков: «Информация» / «Связь с
// администрацией» / «Помощь». Многоуровневое меню (LIST -> подменю с «Назад»),
// по образцу /faction. Сырой SDK не зовётся — только через сервисы; перебор
// игроков для рассылки репорта — через ICore (m_core, есть в BaseSystem).
//
//  * «Информация» — карточка «кто я» (имя/организация/должность из FactionService);
//  * «Связь с администрацией» — INPUT-репорт: текст уходит залогиненным админам
//    онлайн (рассылку делает эта система) и в журнал (ReportService), под
//    кулдауном репорта (источник правды — ReportService);
//  * «Помощь» — тот же help-диалог, что /help (общий слой buildHelpDialogBody).
//
// Кулдаун репорта в ReportService сбрасывается на дисконнекте (иначе утечёт в
// переиспользованный слот — это и баг, и абьюз).
class MenuSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    MenuSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    void showMenu(IPlayer &player);
    void showInfo(IPlayer &player);
    void showReportInput(IPlayer &player);
    void showHelp(IPlayer &player);

    PlayerCommandService &m_commandService;
    FactionService &m_factionService;
    PlayerDialogService &m_dialogService;
    AdminService &m_adminService;
    ReportService &m_reportService;
    PlayerChatService &m_chatService;
};
