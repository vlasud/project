#include "Systems/HelpSystem/HelpDialog.h"

#include <algorithm>
#include <fmt/format.h>
#include <vector>

namespace
{
using HelpCategory = PlayerCommandService::HelpCategory;

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

std::string buildHelpDialogBody(const PlayerCommandService &commands, const FactionService &factions, int playerId)
{
    // Список строит реестр команд (единый источник истины). Клиентский ввод на
    // него не влияет; описания берутся как заявлены при регистрации.
    std::vector<PlayerCommandService::HelpCommand> list = commands.collectHelpCommands();

    // Фракционная секция — только реальному члену организации (серверный факт,
    // не клиентский ввод). Не член — выкидываем фракционные строки целиком.
    const bool inFaction = factions.getMemberFaction(playerId) != FactionService::NO_FACTION;
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

    return body;
}
