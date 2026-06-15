#pragma once

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/FactionService/FactionService.h"
#include <string>

// Построение ТЕЛА help-диалога — общий слой для /help (HelpSystem) и пункта
// «Помощь» меню игрока (MenuSystem). Так оба показывают одно и то же —
// расхождению неоткуда взяться.
//
// Возвращает utf-8 тело для TABLIST_HEADERS («Команда | Описание», жёлтые
// разделители секций по категориям). Кодирование в cp1251 (u()) делает
// вызывающий при сборке Dialog.
//
// Охват: команды реестра с категорией != Hidden; секция «Фракция» — только
// реальному члену организации (getMemberFaction(playerId) != NO_FACTION).
// Клиентский ввод на список не влияет (единый источник истины — реестр).
std::string buildHelpDialogBody(const PlayerCommandService &commands, const FactionService &factions, int playerId);
