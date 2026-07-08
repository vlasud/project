#include "Systems/HelpSystem/HelpSystem.h"

#include "Server/Components/Dialogs/dialogs.hpp"
#include "Systems/HelpSystem/HelpDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <string>

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
    // Тело собирает общий слой (тот же, что у пункта «Помощь» меню /mn) —
    // расхождению неоткуда взяться. Кодирование в cp1251 — здесь, при сборке Dialog.
    const std::string body = buildHelpDialogBody(m_commandService, m_factionService, player.getID());

    Dialog dialog;
    dialog.style = DialogStyle_TABLIST_HEADERS;
    dialog.title = u("Команды");
    dialog.body = u(body);
    dialog.leftButton = u("Закрыть");
    dialog.rightButton = u("");

    // Just-read список: на любой ответ просто закрыть (реакции на выбор нет).
    m_dialogService.show(player, dialog, [](DialogResponse, int, StringView) {});
}
