#include "Systems/Core/HelpSystem/HelpSystem.h"

#include "Server/Components/Dialogs/dialogs.hpp"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <fmt/format.h>
#include <string>
#include <vector>

namespace
{
using HelpCategory = PlayerCommandService::HelpCategory;

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

// Порядок секций /help (фиксированное «оглавление», не зависит от порядка
// регистрации систем). Hidden сюда не доходит (отфильтрован реестром).
int categoryOrder(HelpCategory category)
{
    switch (category)
    {
    case HelpCategory::ChatRP:
        return 0;
    case HelpCategory::Economy:
        return 1;
    case HelpCategory::Faction:
        return 2;
    case HelpCategory::Misc:
        return 3;
    default:
        return 4; // Hidden — не показывается, но порядок задаём детерминированно
    }
}

// Русское имя секции (заголовок-разделитель в tablist).
const char *categoryLabel(HelpCategory category)
{
    switch (category)
    {
    case HelpCategory::ChatRP:
        return "Чат и RP";
    case HelpCategory::Economy:
        return "Экономика";
    case HelpCategory::Faction:
        return "Фракция";
    case HelpCategory::Misc:
        return "Прочее";
    default:
        return "";
    }
}
} // namespace

HelpSystem::HelpSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_commandService(serviceRegister.getService<PlayerCommandService>()),
      m_factionService(serviceRegister.getService<FactionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    m_commandService.add("help", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { cmdHelp(player); },
                         {}, "список доступных вам команд", PlayerCommandService::HelpCategory::Misc);
}

void HelpSystem::cmdHelp(IPlayer &player)
{
    // Список строит реестр команд (единый источник истины). Клиентский ввод на
    // него не влияет; описания берутся как заявлены при регистрации.
    std::vector<PlayerCommandService::HelpCommand> list = m_commandService.collectHelpCommands();

    // Фракционная секция — только реальному члену организации (серверный факт,
    // не клиентский ввод). Не член — выкидываем фракционные строки целиком.
    const bool inFaction = m_factionService.getMemberFaction(player.getID()) != FactionService::NO_FACTION;
    if (!inFaction)
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [](const PlayerCommandService::HelpCommand &cmd)
                                  { return cmd.category == HelpCategory::Faction; }),
                   list.end());

    // Сортировка: по порядку категории, затем по имени — секции в tablist идут
    // фиксированным «оглавлением», внутри секции команды детерминированы.
    std::sort(list.begin(), list.end(),
              [](const PlayerCommandService::HelpCommand &a, const PlayerCommandService::HelpCommand &b)
              {
                  const int oa = categoryOrder(a.category);
                  const int ob = categoryOrder(b.category);
                  return oa != ob ? oa < ob : a.name < b.name;
              });

    // TABLIST_HEADERS: колонки «команда | описание»; жёлтый разделитель отбивает
    // каждую секцию. Первая строка — шапка колонок.
    std::string body = "Команда\tОписание\n";
    int currentOrder = -1;
    for (const PlayerCommandService::HelpCommand &cmd : list)
    {
        const int order = categoryOrder(cmd.category);
        if (order != currentOrder)
        {
            currentOrder = order;
            // Длинное тире вместо U+2500 (нет в cp1251), как в /ahelp.
            body += fmt::format("{{FFB400}}—— {} ——\t\n", categoryLabel(cmd.category));
        }
        body += fmt::format("/{}\t{}\n", cmd.name, cmd.description);
    }
    if (list.empty())
        body += "Нет доступных команд\t\n";
    body.pop_back(); // убрать хвостовой '\n' — иначе пустая строка-фантом в tablist

    Dialog dialog;
    dialog.style = DialogStyle_TABLIST_HEADERS;
    dialog.title = u("Команды");
    dialog.body = u(body);
    dialog.leftButton = u("Закрыть");
    dialog.rightButton = u("");

    // Just-read список: на любой ответ просто закрыть (реакции на выбор нет).
    m_dialogService.show(player, dialog, [](DialogResponse, int, StringView) {});
}
