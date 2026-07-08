#include "Systems/MenuSystem/MenuSystem.h"

#include "Server/Components/Dialogs/dialogs.hpp"
#include "Systems/HelpSystem/HelpDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <fmt/format.h>
#include <string>

namespace
{
// Золотисто-жёлтый админ-канал: репорт читается как служебная строка
// администрации (тег [Репорт] — входящая жалоба ОТ игрока, в отличие от [A]).
const Colour ADMIN_COLOUR = Colour::FromRGBA(0xFFB400FF);
// Голубой системно-информационный тон — подтверждения игроку.
const Colour INFO_COLOUR{120, 220, 255};

constexpr std::size_t REPORT_MAX_BYTES = 180; // utf-8, ~90 кириллических; как /a/an
constexpr std::size_t REPORT_MIN_CHARS = 3;   // минимум видимых символов после чистки

// Число utf-8 кодпойнтов (видимых символов): считаем не-продолжающие байты
// (старшие биты != 10xxxxxx). Текст уже прошёл sanitizeUserText — корректный utf-8.
std::size_t visibleLength(std::string_view utf8)
{
    std::size_t count = 0;
    for (unsigned char c : utf8)
        if ((c & 0xC0) != 0x80)
            ++count;
    return count;
}
} // namespace

MenuSystem::MenuSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_commandService(serviceRegister.getService<PlayerCommandService>()),
      m_factionService(serviceRegister.getService<FactionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_adminService(serviceRegister.getService<AdminService>()),
      m_reportService(serviceRegister.getService<ReportService>()),
      m_chatService(serviceRegister.getService<PlayerChatService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    m_commandService.add("mn", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showMenu(player); },
                         {}, "меню игрока: информация, связь с администрацией, помощь",
                         PlayerCommandService::HelpCategory::Misc);
}

void MenuSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    // Сброс кулдауна репорта: иначе он утечёт в переиспользованный слот.
    m_reportService.reset(player.getID());
}

void MenuSystem::showMenu(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Меню игрока");
    dialog.body = u("Информация\nСвязь с администрацией\nПомощь");
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             // Правая «Закрыть» на корне — закрыть меню совсем.
                             if (response != DialogResponse_Left)
                                 return;
                             switch (listItem)
                             {
                             case 0:
                                 showInfo(*player);
                                 break;
                             case 1:
                                 showReportInput(*player);
                                 break;
                             case 2:
                                 showHelp(*player);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void MenuSystem::showInfo(IPlayer &player)
{
    const int id = player.getID();

    // Данные карточки — серверные факты из FactionService (не клиентский ввод).
    const int factionId = m_factionService.getMemberFaction(id);
    std::string orgValue = "Гражданин"; // без фракции — позитивный RP-статус
    std::string rankValue = "—";        // без должности — тире
    if (factionId != FactionService::NO_FACTION)
    {
        const FactionService::Faction *faction = m_factionService.getFaction(factionId);
        if (faction)
            orgValue = faction->name;
        const FactionService::Rank *rank = m_factionService.getMemberRank(id);
        if (rank)
        {
            rankValue = rank->name;
            // Лидерство — пометкой к должности, не отдельной строкой.
            if (m_factionService.isLeader(id))
                rankValue += " (лидер)";
        }
    }

    // TABLIST_HEADERS: две колонки «Поле | Значение». id игрока в карточке не
    // показываем (это про роль, не про слот).
    std::string body = "Поле\tЗначение\n";
    body += fmt::format("Имя\t{}\n", player.getName().to_string());
    body += fmt::format("Организация\t{}\n", orgValue);
    body += fmt::format("Должность\t{}", rankValue);

    Dialog dialog;
    dialog.style = DialogStyle_TABLIST_HEADERS;
    dialog.title = u("Информация");
    dialog.body = u(body);
    dialog.leftButton = u("Назад");
    dialog.rightButton = u("");

    // Просмотр (выбор строки ничего не делает); «Назад» возвращает в /mn.
    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse, int, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (player)
                                 showMenu(*player);
                         });
}

void MenuSystem::showReportInput(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Связь с администрацией");
    dialog.body = u("Опишите вашу проблему или вопрос. Сообщение увидят администраторы онлайн.");
    dialog.leftButton = u("Отправить");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID()](DialogResponse response, int, StringView inputText)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            const int id = player->getID();

            // Правая «Назад» — обратно в /mn (передумавший остаётся в меню).
            if (response != DialogResponse_Left)
            {
                showMenu(*player);
                return;
            }

            const TimePoint now = std::chrono::steady_clock::now();

            // Кулдаун — источник правды ReportService. Активен -> остаток и выход
            // (диалог закрыт, не зацикливаем).
            if (!m_reportService.ready(id, now))
            {
                player->sendClientMessage(
                    INFO_COLOUR,
                    u(fmt::format("До следующего обращения: {} сек.", m_reportService.secondsLeft(id, now))));
                return;
            }

            // Замученному репорт не даём (репорт идёт мимо общего чат-пути, мут
            // его иначе не глушит). Та же формулировка, что у блока чата.
            if (m_chatService.isMuted(id))
            {
                player->sendClientMessage(
                    INFO_COLOUR,
                    u(fmt::format("Чат заблокирован. Осталось: {} сек", m_chatService.muteSecondsLeft(id))));
                return;
            }

            // Санитизация клиентского ввода: cp1251->utf8, чистка управляющих
            // (включая \t — разделитель tablist) и обрезка по границе utf-8.
            const std::string text = Encoding::sanitizeUserText(
                Encoding::cp1251Toutf8(std::string(inputText.data(), inputText.size())), REPORT_MAX_BYTES);
            if (visibleLength(text) < REPORT_MIN_CHARS)
            {
                player->sendClientMessage(INFO_COLOUR, u("Сообщение слишком короткое"));
                showReportInput(*player); // переоткрываем — ввод не теряется зря
                return;
            }

            const std::string playerName = player->getName().to_string();

            // Рассылка ВСЕМ онлайн залогиненным админам — O(online), тег [Репорт].
            int count = 0;
            const std::string message = u(fmt::format("[Репорт] {}[{}]: {}", playerName, id, text));
            for (IPlayer *other : m_core.getPlayers().entries())
            {
                if (other && m_adminService.isLoggedIn(other->getID()))
                {
                    other->sendClientMessage(ADMIN_COLOUR, message);
                    ++count;
                }
            }

            // Кулдаун ставится ТОЛЬКО на успешную отправку (+ запись в журнал).
            m_reportService.record(id, now, playerName, text);

            if (count > 0)
                player->sendClientMessage(INFO_COLOUR, u("Обращение отправлено администрации. Ожидайте ответа."));
            else
                player->sendClientMessage(
                    INFO_COLOUR, u("Сейчас администраторов нет онлайн. Обращение сохранено, его рассмотрят позже."));
        });
}

void MenuSystem::showHelp(IPlayer &player)
{
    // Тот же общий слой, что /help — пункт «Помощь» и /help показывают одно и то же.
    const std::string body = buildHelpDialogBody(m_commandService, m_factionService, player.getID());

    Dialog dialog;
    dialog.style = DialogStyle_TABLIST_HEADERS;
    dialog.title = u("Команды");
    dialog.body = u(body);
    dialog.leftButton = u("Закрыть");
    dialog.rightButton = u("");

    // Самостоятельный экран: закрытие справки завершает сценарий (в /mn не возвращаем).
    m_dialogService.show(player, dialog, [](DialogResponse, int, StringView) {});
}
