#include "Systems/AdminSystem/AdminSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "ThreadPool/ThreadPool.h"
#include "Utils/Encoding/Encoding.h"
#include "network.hpp"
#include "sodium/crypto_pwhash.h"
#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <optional>
#include <sodium.h>
#include <string>
#include <utility>
#include <vector>

namespace
{
const Colour ADMIN_COLOUR = Colour::FromRGBA(0xFFB400FF);

// Имена уровней (решение геймдизайнера). Индекс = уровень 1..MAX_LEVEL.
const char *LEVEL_NAMES[] = {
    "",                       // 0 — не админ
    "Младший модератор",      // 1
    "Модератор",              // 2
    "Старший модератор",      // 3
    "Администратор",          // 4
    "Главный администратор",  // 5
    "Разработчик",            // 6 — только из БД, без пароля
};

const char *levelName(int level)
{
    if (level < 1 || level > AdminService::MAX_LEVEL)
        return "";
    return LEVEL_NAMES[level];
}

constexpr std::size_t MAX_ADMIN_CHAT_BYTES = 180; // utf-8, ~90 кириллических (как рация)
constexpr std::size_t MIN_ADMIN_PASSWORD = 8;

// Срок бана в днях: границы валидируются ДО записи (клиентскому числу не верим).
// Верх упирается в VARCHAR(128)-причину и здравый смысл; перм-бан — отдельная
// механика, не /ban.
constexpr int MIN_BAN_DAYS = 1;
constexpr int MAX_BAN_DAYS = 365;
constexpr std::size_t MAX_BAN_REASON_BYTES = 120; // utf-8 байт до записи; < VARCHAR(128)

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

// IP игрока (серверный факт) для лога регистрации. Пусто — если адрес не
// сериализуется (на всякий случай, чтобы запись не упала).
std::string playerIp(IPlayer &player)
{
    PeerAddress::AddressString out;
    if (PeerAddress::ToString(player.getNetworkData().networkID.address, out))
    {
        const StringView view = out; // HybridString -> StringView
        return std::string(view.data(), view.size());
    }
    return std::string();
}
} // namespace

AdminSystem::AdminSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_adminService(serviceRegister.getService<AdminService>()),
      m_banService(serviceRegister.getService<BanService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_commandService(serviceRegister.getService<PlayerCommandService>())
{
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session) { loadAdmin(player, session); });
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            m_adminService.reset(player.getID());
            m_pending[player.getID()] = PendingRegistration{};
        });

    auto &commands = m_commandService;

    // /alogin — права None: все проверки внутри (вход в админку доступен тем, у
    // кого есть пароль; единый отказ скрывает, кто админ).
    commands.add("alogin", {{PlayerCommandService::Param::String, "пароль"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 { cmdLogin(player, args.getString(0)); },
                 {}, "войти в админку по паролю", PlayerCommandService::HelpCategory::Hidden);

    // /setadmin — Главный администратор (уровень 5) и выше (Разработчик, 6).
    commands.add("setadmin",
                 {{PlayerCommandService::Param::Int, "id игрока"}, {PlayerCommandService::Param::Int, "уровень"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 { cmdSetAdmin(player, args.getInt(0), args.getInt(1)); },
                 PermissionSpec::admin(AdminService::MAX_ASSIGNABLE_LEVEL), "выдать или снять уровень админки (0–5)",
                 PlayerCommandService::HelpCategory::Hidden);

    // /a — админ-чат (уровень 1+).
    commands.add("a", {{PlayerCommandService::Param::String, "текст"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 { cmdAdminChat(player, args.getString(0)); },
                 PermissionSpec::admin(1), "админ-чат: написать всем админам онлайн",
                 PlayerCommandService::HelpCategory::Hidden);

    // /kick — кик игрока (уровень 1+). Причина — обязательный параметр команды.
    commands.add("kick",
                 {{PlayerCommandService::Param::Int, "id игрока"}, {PlayerCommandService::Param::String, "причина"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 { cmdKick(player, args.getInt(0), args.getString(1)); },
                 PermissionSpec::admin(1), "кикнуть игрока с сервера с указанием причины",
                 PlayerCommandService::HelpCategory::Hidden);

    // /ban — бан аккаунта на дни (уровень 3+). Причина обязательна (жадный
    // последний параметр).
    commands.add("ban",
                 {{PlayerCommandService::Param::Int, "id игрока"},
                  {PlayerCommandService::Param::Int, "дней"},
                  {PlayerCommandService::Param::String, "причина"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 { cmdBan(player, args.getInt(0), args.getInt(1), args.getString(2)); },
                 PermissionSpec::admin(3), "забанить аккаунт на срок в днях с указанием причины",
                 PlayerCommandService::HelpCategory::Hidden);

    // /ahelp — список доступных игроку админ-команд (любой залогиненный админ,
    // уровень 1+). Список строит резолвер по эфф. уровню: видно ровно то, что
    // доступно «прямо сейчас» (разработчик с эфф. 6 — весь dev-тулинг).
    commands.add("ahelp", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { cmdAdminHelp(player); },
                 PermissionSpec::admin(1), "список доступных вам админ-команд",
                 PlayerCommandService::HelpCategory::Hidden);
}

void AdminSystem::loadAdmin(IPlayer &player, const PlayerSessionService::Session &session)
{
    // Запрос И вычитка — на воркере; на главный поток приходит владеющий
    // optional<level, hash>. Нет строки → не админ (level 0, кэш уже нулевой
    // после reset на конце прошлой сессии / дефолта слота).
    DatabaseManager::selectQuery<std::optional<std::pair<int, std::string>>>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult result = schema.getTable("admin_account")
                                           .select("level", "password_hash")
                                           .where("account_id = :account")
                                           .limit(1)
                                           .bind("account", accountId)
                                           .execute();
            std::optional<std::pair<int, std::string>> row;
            if (mysqlx::Row r = result.fetchOne())
            {
                // level читаем как int64 и сужаем: значение вне диапазона int не
                // должно ронять get<int>() (битый ряд иначе оборвал бы загрузку).
                const int level = static_cast<int>(r.get(0).get<std::int64_t>());
                // password_hash может быть NULL (выдан уровень без пароля).
                std::string hash;
                if (!r.get(1).isNull())
                    hash = r.get(1).get<std::string>();
                row = std::make_pair(level, std::move(hash));
            }
            return row;
        },
        [this, playerId = player.getID(), serial = session.serial](std::optional<std::pair<int, std::string>> row)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/другая сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            if (!m_core.getPlayers().get(playerId))
                return;
            if (!row)
                return; // не админ — слот остаётся нулевым

            const bool registered = !row->second.empty();
            m_adminService.cacheLoaded(playerId, row->first, registered, std::move(row->second));
        },
        [](const std::string &error) { LogManager::log(Error, "AdminSystem: failed to load admin: " + error); });
}

void AdminSystem::cmdLogin(IPlayer &player, StringView password)
{
    const int id = player.getID();
    // Вход только в активной сессии (аккаунт известен).
    if (!m_sessionService.isActive(id))
        return;

    if (m_adminService.isLoggedIn(id))
    {
        player.sendClientMessage(ADMIN_COLOUR, u("Вы уже вошли в админку"));
        return;
    }

    const TimePoint now = std::chrono::steady_clock::now();
    if (m_adminService.isLockedOut(id, now))
    {
        player.sendClientMessage(
            ADMIN_COLOUR,
            u(fmt::format("Слишком много попыток входа. Подождите {} сек.", m_adminService.lockoutSecondsLeft(id, now))));
        return;
    }

    // Единый отказ для «не админ / нет пароля»: не палим, что аккаунт не админ.
    if (!m_adminService.isRegistered(id) || m_adminService.getStoredLevel(id) == 0)
    {
        m_adminService.registerFailedLogin(id, now);
        player.sendClientMessage(ADMIN_COLOUR, u("Неверный пароль"));
        return;
    }

    // Верификация — на ВОРКЕРЕ (Argon2id дорогой); в колбэке serial-guard.
    const PlayerSessionService::Session *session = m_sessionService.get(id);
    const std::uint32_t serial = session ? session->serial : 0;

    ThreadPool::Task<bool> task;
    task.func = [password = std::string(password.data(), password.size()), hash = m_adminService.passwordHash(id)]()
    { return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) == 0; };
    task.callback = [this, playerId = id, serial](bool verified)
    {
        const PlayerSessionService::Session *current = m_sessionService.get(playerId);
        if (!current || current->serial != serial)
            return; // другая сессия в слоте — результат не наш
        IPlayer *player = m_core.getPlayers().get(playerId);
        if (!player)
            return;

        const TimePoint now = std::chrono::steady_clock::now();
        if (!verified)
        {
            m_adminService.registerFailedLogin(playerId, now);
            player->sendClientMessage(ADMIN_COLOUR, u("Неверный пароль"));
            return;
        }

        m_adminService.setLoggedIn(playerId, true);
        m_adminService.clearLoginFails(playerId);
        const int level = m_adminService.getStoredLevel(playerId);
        player->sendClientMessage(
            ADMIN_COLOUR, u(fmt::format("Вход в админку выполнен. Уровень: {} — {}", level, levelName(level))));
    };

    ThreadPool::addTask(std::move(task));
}

void AdminSystem::cmdSetAdmin(IPlayer &actor, int targetId, int level)
{
    IPlayer *target = m_core.getPlayers().get(targetId);
    if (!target || !m_sessionService.isActive(targetId))
    {
        actor.sendClientMessage(ADMIN_COLOUR, u("Игрок не найден"));
        return;
    }
    if (targetId == actor.getID())
    {
        actor.sendClientMessage(ADMIN_COLOUR, u("Нельзя изменить собственный уровень"));
        return;
    }
    // Диапазон /setadmin — 0..5: уровень 6 («Разработчик») командой не выдаётся,
    // он ставится только прямым INSERT в БД.
    if (level < 0 || level > AdminService::MAX_ASSIGNABLE_LEVEL)
    {
        actor.sendClientMessage(ADMIN_COLOUR, u("Уровень должен быть от 0 до 5"));
        return;
    }
    // Иерархия по СОХРАНЁННОМУ уровню (а не эффективному): нельзя трогать равного
    // или старшего, даже если тот не залогинен в админку.
    if (m_adminService.getStoredLevel(targetId) >= m_adminService.getStoredLevel(actor.getID()))
    {
        actor.sendClientMessage(ADMIN_COLOUR, u("Цель равна вам по уровню или выше"));
        return;
    }

    const PlayerSessionService::AccountId targetAccount = m_sessionService.getAccountId(targetId);
    const std::string actorName = actor.getName().to_string();
    const std::string targetName = target->getName().to_string();

    if (level == 0)
    {
        m_adminService.setLevel(*target, targetAccount, 0);
        actor.sendClientMessage(ADMIN_COLOUR, u(fmt::format("{}[{}] снят с админки", targetName, targetId)));
        target->sendClientMessage(ADMIN_COLOUR, u("Вы сняты с админки"));
        logAdminAction(fmt::format("{} снял {}[{}] с админки", actorName, targetName, targetId));
        return;
    }

    if (m_adminService.isRegistered(targetId))
    {
        m_adminService.setLevel(*target, targetAccount, level);
        actor.sendClientMessage(
            ADMIN_COLOUR, u(fmt::format("{}[{}] назначен уровень {} — {}", targetName, targetId, level, levelName(level))));
        target->sendClientMessage(
            ADMIN_COLOUR, u(fmt::format("Вам выдан уровень админки {} — {}", level, levelName(level))));
        logAdminAction(fmt::format("{} выдал {}[{}] уровень {} — {}", actorName, targetName, targetId, level,
                                   levelName(level)));
        return;
    }

    // Цель ещё не регистрирована — запускаем диалоговую регистрацию пароля.
    startRegistration(*target, level);
    actor.sendClientMessage(ADMIN_COLOUR,
                            u(fmt::format("{}[{}] предложена регистрация админки (уровень {})", targetName, targetId,
                                          level)));
}

void AdminSystem::cmdAdminChat(IPlayer &player, StringView rawText)
{
    // Ввод клиента: cp1251 -> utf-8, чистка и обрезка без разрыва символа.
    const std::string text = Encoding::sanitizeUserText(
        Encoding::cp1251Toutf8(std::string(rawText.data(), rawText.size())), MAX_ADMIN_CHAT_BYTES);
    if (text.empty())
        return;

    const int id = player.getID();
    const int level = m_adminService.getStoredLevel(id);
    const std::string message =
        u(fmt::format("[A] [{}] {}[{}]: {}", levelName(level), player.getName().to_string(), id, text));

    // Рассылка всем онлайн залогиненным админам — O(online).
    for (IPlayer *other : m_core.getPlayers().entries())
    {
        if (other && m_adminService.isLoggedIn(other->getID()))
            other->sendClientMessage(ADMIN_COLOUR, message);
    }
}

void AdminSystem::cmdKick(IPlayer &actor, int targetId, StringView rawReason)
{
    IPlayer *target = m_core.getPlayers().get(targetId);
    if (!target)
    {
        actor.sendClientMessage(ADMIN_COLOUR, u("Игрок не найден"));
        return;
    }
    // Иерархия: нельзя кикнуть равного/старшего по сохранённому уровню.
    if (m_adminService.getStoredLevel(targetId) >= m_adminService.getStoredLevel(actor.getID()))
    {
        actor.sendClientMessage(ADMIN_COLOUR, u("Нельзя применить к этому игроку"));
        return;
    }

    const std::string reason = Encoding::sanitizeUserText(
        Encoding::cp1251Toutf8(std::string(rawReason.data(), rawReason.size())), MAX_ADMIN_CHAT_BYTES);

    const std::string actorName = actor.getName().to_string();
    const std::string targetName = target->getName().to_string();
    const std::string reasonText = reason.empty() ? "не указана" : reason;

    logAdminAction(fmt::format("{} кикнул {}[{}]. Причина: {}", actorName, targetName, targetId, reasonText));
    target->sendClientMessage(ADMIN_COLOUR, u(fmt::format("Вы были кикнуты с сервера. Причина: {}", reasonText)));
    target->kick();
}

void AdminSystem::cmdBan(IPlayer &actor, int targetId, int days, StringView rawReason)
{
    IPlayer *target = m_core.getPlayers().get(targetId);
    // Сессия цели обязательна: бан пишется по accountId (источник правды связки).
    if (!target || !m_sessionService.isActive(targetId))
    {
        actor.sendClientMessage(ADMIN_COLOUR, u("Игрок не найден"));
        return;
    }
    if (targetId == actor.getID())
    {
        actor.sendClientMessage(ADMIN_COLOUR, u("Нельзя применить к этому игроку"));
        return;
    }
    // Иерархия по СОХРАНЁННОМУ уровню: нельзя забанить равного/старшего.
    if (m_adminService.getStoredLevel(targetId) >= m_adminService.getStoredLevel(actor.getID()))
    {
        actor.sendClientMessage(ADMIN_COLOUR, u("Нельзя применить к этому игроку"));
        return;
    }
    // Срок валидируем ДО записи — клиентскому числу не верим.
    if (days < MIN_BAN_DAYS || days > MAX_BAN_DAYS)
    {
        actor.sendClientMessage(ADMIN_COLOUR, u("Срок бана должен быть от 1 до 365 дней"));
        return;
    }

    // accountId/IP — серверные факты (сессия + networkData), не из ввода клиента.
    const PlayerSessionService::AccountId targetAccount = m_sessionService.getAccountId(targetId);
    const PlayerSessionService::AccountId actorAccount = m_sessionService.getAccountId(actor.getID());
    const std::string ip = playerIp(*target);

    const std::string reason = Encoding::sanitizeUserText(
        Encoding::cp1251Toutf8(std::string(rawReason.data(), rawReason.size())), MAX_BAN_REASON_BYTES);
    const std::string reasonText = reason.empty() ? "не указана" : reason;

    const std::string actorName = actor.getName().to_string();
    const std::string targetName = target->getName().to_string();

    m_banService.banAccount(targetAccount, days, reasonText, actorAccount, ip);

    logAdminAction(fmt::format("{}[{}] забанил {}[{}] на {} дн. Причина: {}", actorName, actor.getID(), targetName,
                               targetId, days, reasonText));
    target->sendClientMessage(ADMIN_COLOUR, u(fmt::format("Вы забанены на {} дн. Причина: {}", days, reasonText)));
    target->kick();
}

void AdminSystem::cmdAdminHelp(IPlayer &player)
{
    // Список строит резолвер по ЭФФЕКТИВНОМУ уровню игрока — ровно то, что ему
    // доступно «прямо сейчас». Клиентский ввод на список не влияет.
    std::vector<PlayerCommandService::AccessibleCommand> list = m_commandService.collectAccessible(player);

    // Сортировка: по уровню (возр.), затем по имени — группировка по уровню в
    // tablist получается естественной.
    std::sort(list.begin(), list.end(),
              [](const PlayerCommandService::AccessibleCommand &a, const PlayerCommandService::AccessibleCommand &b)
              { return a.adminLevel != b.adminLevel ? a.adminLevel < b.adminLevel : a.name < b.name; });

    // TABLIST_HEADERS: колонки «команда | описание»; строка-заголовок уровня
    // отбивает каждую группу (уровень виден явно). Первая строка — шапка колонок.
    std::string body = "Команда\tОписание\n";
    int currentLevel = -1;
    for (const PlayerCommandService::AccessibleCommand &cmd : list)
    {
        if (cmd.adminLevel != currentLevel)
        {
            currentLevel = cmd.adminLevel;
            // Жёлтый разделитель группы (цвет админ-канала) — заголовки уровней
            // всплывают над белыми строками. Длинное тире вместо U+2500 (нет в cp1251).
            body += fmt::format("{{FFB400}}—— {} (ур. {}) ——\t\n", levelName(currentLevel), currentLevel);
        }
        body += fmt::format("/{}\t{}\n", cmd.name, cmd.description);
    }
    if (list.empty())
        body += "Нет доступных команд\t\n";
    body.pop_back(); // убрать хвостовой '\n' — иначе пустая строка-фантом в tablist

    Dialog dialog;
    dialog.style = DialogStyle_TABLIST_HEADERS;
    dialog.title = u("Админ-команды");
    dialog.body = u(body);
    dialog.leftButton = u("Закрыть");
    dialog.rightButton = u("");

    // Just-read список: на любой ответ просто закрыть (реакции на выбор нет).
    m_dialogService.show(player, dialog, [](DialogResponse, int, StringView) {});
}

// ------------------------------------------------------------- регистрация админа

void AdminSystem::startRegistration(IPlayer &target, int level)
{
    const int id = target.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(id);
    if (!session)
        return;

    PendingRegistration &pending = m_pending[id];
    pending.active = true;
    pending.level = level;
    pending.firstPassword.clear();
    pending.serial = session->serial;

    target.sendClientMessage(ADMIN_COLOUR, u(fmt::format("Вам назначают админку уровня {} — {}", level,
                                                         levelName(level))));
    showRegisterPasswordDialog(target);
}

void AdminSystem::showRegisterPasswordDialog(IPlayer &target)
{
    Dialog dialog;
    dialog.style = DialogStyle_PASSWORD;
    dialog.title = u("Регистрация админа — Пароль");
    dialog.body = u("Придумайте админ-пароль. Минимум 8 символов");
    dialog.leftButton = u("Далее");
    dialog.rightButton = u("Отмена");

    m_dialogService.show(
        target, dialog,
        [this, playerId = target.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            PendingRegistration &pending = m_pending[playerId];
            // Serial-guard цели: в слот мог сесть другой игрок, пока диалог открыт
            // (serial запомнен в startRegistration).
            const PlayerSessionService::Session *session = m_sessionService.get(playerId);
            if (!pending.active || !session || session->serial != pending.serial)
                return;

            if (response == DialogResponse_Right)
            {
                // Отмена назначения.
                pending = PendingRegistration{};
                player->sendClientMessage(ADMIN_COLOUR, u("Регистрация админки отменена"));
                return;
            }

            if (text.size() < MIN_ADMIN_PASSWORD)
            {
                player->sendClientMessage(ADMIN_COLOUR, u("Минимум 8 символов!"));
                showRegisterPasswordDialog(*player);
                return;
            }

            pending.firstPassword = std::string(text.data(), text.size());
            showRegisterConfirmDialog(*player);
        });
}

void AdminSystem::showRegisterConfirmDialog(IPlayer &target)
{
    Dialog dialog;
    dialog.style = DialogStyle_PASSWORD;
    dialog.title = u("Регистрация админа — Подтверждение");
    dialog.body = u("Повторите админ-пароль");
    dialog.leftButton = u("Далее");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        target, dialog,
        [this, playerId = target.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            PendingRegistration &pending = m_pending[playerId];
            const PlayerSessionService::Session *session = m_sessionService.get(playerId);
            if (!pending.active || !session || session->serial != pending.serial)
                return;

            if (response == DialogResponse_Right)
            {
                showRegisterPasswordDialog(*player); // назад к шагу 1
                return;
            }

            if (std::string(text.data(), text.size()) != pending.firstPassword)
            {
                player->sendClientMessage(ADMIN_COLOUR, u("Пароли не совпадают"));
                showRegisterPasswordDialog(*player);
                return;
            }

            finishRegistration(*player);
        });
}

void AdminSystem::finishRegistration(IPlayer &target)
{
    const int id = target.getID();
    PendingRegistration &pending = m_pending[id];
    const PlayerSessionService::Session *session = m_sessionService.get(id);
    if (!pending.active || !session || session->serial != pending.serial)
        return;

    const PlayerSessionService::AccountId accountId = session->accountId;
    const std::uint32_t serial = session->serial;
    const int level = pending.level;
    std::string password = std::move(pending.firstPassword);
    const std::string ip = playerIp(target);
    pending = PendingRegistration{}; // диалоги завершены, состояние больше не нужно

    m_adminService.finalizeRegistration(
        accountId, level, std::move(password), ip,
        [this, playerId = id, serial, level](std::string hash)
        {
            // Serial-guard: в слот мог сесть другой игрок, пока шёл async-хеш/запись.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;

            // Сидируем кэш и авто-логиним: цель только что доказала пароль.
            m_adminService.cacheLoaded(playerId, level, true, std::move(hash));
            m_adminService.setLoggedIn(playerId, true);
            m_adminService.clearLoginFails(playerId);
            player->sendClientMessage(
                ADMIN_COLOUR, u(fmt::format("Вы зарегистрированы как админ. Уровень: {} — {}", level, levelName(level))));
        });
}

void AdminSystem::logAdminAction(const std::string &utf8Line)
{
    // В админ-чат всем залогиненным админам (канал [A]) и в файл-лог.
    const std::string chat = u(fmt::format("[A] {}", utf8Line));
    for (IPlayer *other : m_core.getPlayers().entries())
    {
        if (other && m_adminService.isLoggedIn(other->getID()))
            other->sendClientMessage(ADMIN_COLOUR, chat);
    }
    LogManager::log(Message, "[ADMIN] " + utf8Line);
}
