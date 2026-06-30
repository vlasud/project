#include "Systems/FamilySystem/FamilySystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};
constexpr std::size_t MAX_FAMILY_BYTES = 180; // utf-8, ~90 кириллических

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

// Текст ошибки операции членства для клиента (utf-8).
const char *resultError(FamilyService::Result result)
{
    switch (result)
    {
    case FamilyService::Result::InvalidName:
        return "Название должно быть от 3 до 24 символов без спецсимволов";
    case FamilyService::Result::NameTaken:
        return "Семья с таким названием уже существует";
    case FamilyService::Result::AlreadyInFamily:
        return "Вы уже состоите в семье";
    case FamilyService::Result::FamilyFull:
        return "В семье достигнут лимит участников";
    case FamilyService::Result::NotFound:
        return "Семья не найдена";
    case FamilyService::Result::NoSession:
        return "Вы ещё не авторизованы";
    case FamilyService::Result::NotLoaded:
        return "Семьи ещё загружаются, попробуйте через момент";
    case FamilyService::Result::Ok:
        return "";
    }
    return "";
}
} // namespace

FamilySystem::FamilySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_familyService(serviceRegister.getService<FamilyService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_chatService(serviceRegister.getService<PlayerChatService>()),
      m_houseService(serviceRegister.getService<HouseService>())
{
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        { m_familyService.onSessionStart(player.getID(), session.accountId); });
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        { m_familyService.onSessionEnd(player.getID()); });

    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add("family", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showMenu(player); },
                 {}, "меню семьи: создать, состав, пригласить, выйти", PlayerCommandService::HelpCategory::Misc);

    commands.add("f", {{PlayerCommandService::Param::String, "текст"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 { familyChat(player, args.getString(0)); },
                 {}, "чат семьи: написать всем в своей семье", PlayerCommandService::HelpCategory::Misc);
}

void FamilySystem::initialize(IComponentList *components)
{
    loadAll();
}

// ------------------------------------------------------------------ загрузка

void FamilySystem::loadAll()
{
    // Семьи грузим первыми, затем членов (loadMember требует существующих семей).
    // Кортеж: id, name, owner_account_id, created_at.
    using FamilyRow = std::tuple<int, std::string, FamilyService::AccountId, std::int64_t>;
    DatabaseManager::selectQuery<std::vector<FamilyRow>>(
        [](mysqlx::Schema schema)
        {
            mysqlx::RowResult rows =
                schema.getTable("family").select("id", "name", "owner_account_id", "created_at").execute();
            std::vector<FamilyRow> result;
            while (mysqlx::Row row = rows.fetchOne())
            {
                result.emplace_back(row.get(0).get<int>(), row.get(1).get<std::string>(),
                                    row.get(2).get<FamilyService::AccountId>(), row.get(3).get<std::int64_t>());
            }
            return result;
        },
        [this](std::vector<FamilyRow> families)
        {
            for (auto &[id, name, ownerAccountId, createdAt] : families)
                m_familyService.loadFamily(id, std::move(name), ownerAccountId, static_cast<long long>(createdAt));

            // Члены — строго после всех семей.
            using MemberRow = std::tuple<int, FamilyService::AccountId, std::string, std::int64_t>;
            DatabaseManager::selectQuery<std::vector<MemberRow>>(
                [](mysqlx::Schema schema)
                {
                    mysqlx::RowResult rows = schema.getTable("family_member")
                                                 .select("family_id", "account_id", "name", "joined_at")
                                                 .execute();
                    std::vector<MemberRow> result;
                    while (mysqlx::Row row = rows.fetchOne())
                    {
                        result.emplace_back(row.get(0).get<int>(), row.get(1).get<FamilyService::AccountId>(),
                                            row.get(2).get<std::string>(), row.get(3).get<std::int64_t>());
                    }
                    return result;
                },
                [this](std::vector<MemberRow> members)
                {
                    for (auto &[familyId, accountId, name, joinedAt] : members)
                        m_familyService.loadMember(familyId, accountId, std::move(name),
                                                   static_cast<long long>(joinedAt));
                    m_familyService.finalizeLoad();
                    // Сессии могли стартовать до прихода этого колбэка (загрузка
                    // асинхронна) — резолвим членство уже подключённым игрокам.
                    for (IPlayer *player : m_core.getPlayers().entries())
                    {
                        const PlayerSessionService::Session *session = m_sessionService.get(player->getID());
                        if (session)
                            m_familyService.onSessionStart(player->getID(), session->accountId);
                    }
                },
                [](const std::string &error)
                { LogManager::log(Error, "FamilySystem: failed to load family members: " + error); });
        },
        [](const std::string &error)
        { LogManager::log(Error, "FamilySystem: failed to load families: " + error); });
}

// ------------------------------------------------------------------ чат /f

void FamilySystem::familyChat(IPlayer &player, StringView rawText)
{
    const int playerId = player.getID();
    const int familyId = m_familyService.getFamilyId(playerId);
    const FamilyService::Family *family = m_familyService.getFamily(familyId);
    if (!family)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы не состоите в семье. Чат семьи — команда /f"));
        return;
    }

    // Ввод клиента: cp1251 -> utf-8, чистка управляющих и обрезка без разрыва
    // символа, затем обезвреживание цветокодов '{','}','~' — их раскрашивает
    // КЛИЕНТ при рендере client message (иначе игрок подделает цвет строки). Имя
    // семьи в теге так же защищено sanitizeName при создании.
    const std::string text = Encoding::neutralizeColorCodes(Encoding::sanitizeUserText(
        Encoding::cp1251Toutf8(std::string(rawText.data(), rawText.size())), MAX_FAMILY_BYTES));
    if (text.empty())
        return; // пустое — молча выйти

    // Антиспам/мут — общий барьер чата (как обычный чат и /r). В tryChat уходит
    // тот же текст, что и в рассылку (санитизированный) — как делает ChatSystem.
    const PlayerChatService::Check check = m_chatService.tryChat(playerId, text, std::chrono::steady_clock::now());
    switch (check.block)
    {
    case PlayerChatService::Block::Muted:
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Чат заблокирован. Осталось: {} сек", check.secondsLeft)));
        return;
    case PlayerChatService::Block::TooFast:
        player.sendClientMessage(ERROR_COLOUR, u("Не так быстро! Подождите немного"));
        return;
    case PlayerChatService::Block::Duplicate:
        player.sendClientMessage(ERROR_COLOUR, u("Не повторяйтесь"));
        return;
    case PlayerChatService::Block::None:
        break;
    }

    const std::string message =
        u(fmt::format("[{}] {}[{}] : {}", family->name, player.getName().to_string(), playerId, text));

    // Один проход по онлайн-игрокам O(online): фильтр по членству в этой семье.
    for (IPlayer *member : m_core.getPlayers().entries())
    {
        if (m_familyService.getFamilyId(member->getID()) == familyId)
            member->sendClientMessage(Colour::White(), message);
    }
}

// ------------------------------------------------------------------ меню /family

std::vector<FamilySystem::Action> FamilySystem::buildActions(int playerId) const
{
    std::vector<Action> actions;
    if (m_familyService.getFamilyId(playerId) == FamilyService::NO_FAMILY)
    {
        actions.push_back(Action::Create);
        return actions;
    }
    actions.push_back(Action::Roster);
    if (m_familyService.isOwner(playerId))
        actions.push_back(Action::Invite);
    actions.push_back(Action::Leave);
    if (m_familyService.isOwner(playerId))
        actions.push_back(Action::Disband);
    return actions;
}

void FamilySystem::showMenu(IPlayer &player)
{
    const int playerId = player.getID();
    const std::vector<Action> actions = buildActions(playerId);
    const FamilyService::Family *family = m_familyService.getFamily(m_familyService.getFamilyId(playerId));

    std::string body;
    for (const Action action : actions)
    {
        switch (action)
        {
        case Action::Create:
            body += "Создать семью\n";
            break;
        case Action::Roster:
            body += "Состав семьи\n";
            break;
        case Action::Invite:
            body += "Пригласить игрока\n";
            break;
        case Action::Leave:
            body += "Выйти из семьи\n";
            break;
        case Action::Disband:
            body += "Распустить семью\n";
            break;
        }
    }
    if (!body.empty())
        body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(family ? fmt::format("Семья «{}»", family->name) : std::string("Семья"));
    dialog.body = u(body);
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(
        player, dialog,
        [this, playerId, actions](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;
            // Состояние могло измениться, пока диалог открыт — пересобираем
            // ДОСТУПНЫЕ действия и сверяем выбранный пункт с актуальным набором.
            const std::vector<Action> current = buildActions(playerId);
            if (listItem < 0 || static_cast<std::size_t>(listItem) >= actions.size())
                return;
            const Action chosen = actions[listItem];
            // Действие должно остаться доступным (напр. владельца сменили).
            if (std::find(current.begin(), current.end(), chosen) == current.end())
            {
                showMenu(*player);
                return;
            }
            switch (chosen)
            {
            case Action::Create:
                showCreateInput(*player);
                break;
            case Action::Roster:
                showRoster(*player);
                break;
            case Action::Invite:
                showInviteInput(*player);
                break;
            case Action::Leave:
                showLeaveConfirm(*player);
                break;
            case Action::Disband:
                showDisbandConfirm(*player);
                break;
            }
        });
}

void FamilySystem::showCreateInput(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Создание семьи");
    dialog.body = u(fmt::format("Введите название семьи ({}-{} символов).\nСоздание бесплатно.",
                                FamilyService::NAME_MIN, FamilyService::NAME_MAX));
    dialog.leftButton = u("Создать");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID()](DialogResponse response, int, StringView input)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showMenu(*player);
                return;
            }
            const PlayerSessionService::Session *session = m_sessionService.get(playerId);
            if (!session)
            {
                player->sendClientMessage(ERROR_COLOUR, u(resultError(FamilyService::Result::NoSession)));
                return;
            }
            // Гейт: создать семью можно только при наличии дома в собственности.
            // Ключ владельца — серверный accountId, как формирует HouseSystem
            // (std::to_string(accountId)); клиенту не доверяем.
            if (!m_houseService.ownsHouse(std::to_string(session->accountId)))
            {
                player->sendClientMessage(
                    ERROR_COLOUR, u("Чтобы создать семью, нужен свой дом. Займите свободный дом на карте"));
                return;
            }
            // Ввод клиента: cp1251 -> utf-8; чистку/валидацию делает сервис.
            const std::string name = Encoding::cp1251Toutf8(std::string(input.data(), input.size()));
            const FamilyService::Result result = m_familyService.createFamily(*player, session->accountId, name);
            if (result != FamilyService::Result::Ok)
            {
                player->sendClientMessage(ERROR_COLOUR, u(resultError(result)));
                if (result == FamilyService::Result::InvalidName || result == FamilyService::Result::NameTaken)
                    showCreateInput(*player); // повтор ввода
                return;
            }
            const FamilyService::Family *family =
                m_familyService.getFamily(m_familyService.getFamilyId(playerId));
            player->sendClientMessage(
                INFO_COLOUR, u(fmt::format("Семья «{}» создана. Вы — старейшина. Чат семьи — команда /f",
                                           family ? family->name : "")));
        });
}

void FamilySystem::showRoster(IPlayer &player)
{
    const int playerId = player.getID();
    const FamilyService::Family *family = m_familyService.getFamily(m_familyService.getFamilyId(playerId));
    if (!family)
    {
        showMenu(player);
        return;
    }

    // Имя владельца берём из состава (он всегда член).
    std::string ownerName = "?";
    for (const FamilyService::Mem &member : family->members)
    {
        if (member.accountId == family->ownerAccountId)
        {
            ownerName = member.name;
            break;
        }
    }

    const PlayerSessionService::Session *mySession = m_sessionService.get(playerId);
    const FamilyService::AccountId myAccount =
        mySession ? mySession->accountId : PlayerSessionService::NO_ACCOUNT;

    std::string body = fmt::format("Семья: {}\nВладелец: {}\n\nСостав ({}/{}):\n", family->name, ownerName,
                                   family->members.size(), FamilyService::MAX_MEMBERS);
    // Порядок по стажу (members уже отсортированы по joinedAt). Онлайн/офлайн не
    // показываем; владелец помечен «— старейшина», себя — «(вы)».
    for (const FamilyService::Mem &member : family->members)
    {
        const char *ownerMark = member.accountId == family->ownerAccountId ? " — старейшина" : "";
        const char *youMark = (myAccount != PlayerSessionService::NO_ACCOUNT && member.accountId == myAccount)
                                  ? " (вы)"
                                  : "";
        body += fmt::format("{}{}{}\n", member.name, ownerMark, youMark);
    }

    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u(fmt::format("Семья «{}»", family->name));
    dialog.body = u(body);
    dialog.leftButton = u("Назад");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(player, dialog,
                         [this, playerId](DialogResponse response, int, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response == DialogResponse_Left)
                                 showMenu(*player);
                         });
}

void FamilySystem::showInviteInput(IPlayer &player)
{
    if (!m_familyService.isOwner(player.getID()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Это может только владелец семьи"));
        return;
    }

    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Приглашение в семью");
    dialog.body = u("Введите id игрока для приглашения.\nИгрок должен быть в сети и не состоять в семье.");
    dialog.leftButton = u("Пригласить");
    dialog.rightButton = u("Назад");

    m_dialogService.showNumberInput(
        player, dialog,
        [this, ownerId = player.getID()](DialogResponse response, std::int64_t targetIdRaw)
        {
            IPlayer *owner = m_core.getPlayers().get(ownerId);
            if (!owner)
                return;
            if (response != DialogResponse_Left)
            {
                showMenu(*owner);
                return;
            }
            if (!m_familyService.isOwner(ownerId))
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Это может только владелец семьи"));
                return;
            }
            const int familyId = m_familyService.getFamilyId(ownerId);
            const FamilyService::Family *family = m_familyService.getFamily(familyId);
            if (!family)
                return;
            if (family->members.size() >= FamilyService::MAX_MEMBERS)
            {
                owner->sendClientMessage(ERROR_COLOUR, u(resultError(FamilyService::Result::FamilyFull)));
                return;
            }

            // id из клиентского ввода — bounds + онлайн + не сам приглашающий +
            // не в семье + авторизован.
            const int targetId = static_cast<int>(targetIdRaw);
            if (targetId == ownerId)
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Нельзя пригласить самого себя"));
                showInviteInput(*owner);
                return;
            }
            IPlayer *target = m_core.getPlayers().get(targetId);
            const PlayerSessionService::Session *targetSession = target ? m_sessionService.get(targetId) : nullptr;
            if (!target || !targetSession)
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Игрок не найден или не авторизован"));
                showInviteInput(*owner);
                return;
            }
            if (m_familyService.getFamilyId(targetId) != FamilyService::NO_FAMILY)
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Игрок уже состоит в семье"));
                return;
            }

            owner->sendClientMessage(INFO_COLOUR, u(fmt::format("Приглашение отправлено игроку {}",
                                                                target->getName().to_string())));
            // Приглашённому — диалог подтверждения. Принятие перепроверит всё на
            // момент клика (serial приглашающего ловит его пере-вход в слот).
            const PlayerSessionService::Session *ownerSession = m_sessionService.get(ownerId);
            showInviteConfirm(*target, ownerId, familyId, ownerSession ? ownerSession->serial : 0);
        });
}

void FamilySystem::showInviteConfirm(IPlayer &invited, int inviterId, int familyId, std::uint32_t inviterSerial)
{
    const FamilyService::Family *family = m_familyService.getFamily(familyId);
    IPlayer *inviter = m_core.getPlayers().get(inviterId);
    if (!family || !inviter)
        return;

    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u("Приглашение в семью");
    dialog.body = u(fmt::format("{} приглашает вас в семью «{}». Принять приглашение?",
                                inviter->getName().to_string(), family->name));
    dialog.leftButton = u("Принять");
    dialog.rightButton = u("Отклонить");

    m_dialogService.show(
        invited, dialog,
        [this, invitedId = invited.getID(), inviterId, familyId, inviterSerial](DialogResponse response, int,
                                                                                StringView)
        {
            IPlayer *invited = m_core.getPlayers().get(invitedId);
            if (!invited || response != DialogResponse_Left)
                return;

            const PlayerSessionService::Session *invitedSession = m_sessionService.get(invitedId);
            if (!invitedSession)
            {
                invited->sendClientMessage(ERROR_COLOUR, u("Вы ещё не авторизованы"));
                return;
            }
            // Перепроверка на момент СОГЛАСИЯ (не доверяем старому диалогу):
            // семья ещё есть, не переполнена, цель не в семье, приглашавший — всё
            // ещё владелец ЭТОЙ семьи и это та же его сессия (serial).
            const FamilyService::Family *family = m_familyService.getFamily(familyId);
            const PlayerSessionService::Session *inviterSession = m_sessionService.get(inviterId);
            const bool inviterValid = inviterSession && inviterSession->serial == inviterSerial &&
                                      m_familyService.isOwner(inviterId) &&
                                      m_familyService.getFamilyId(inviterId) == familyId;
            if (!family || !inviterValid)
            {
                invited->sendClientMessage(ERROR_COLOUR, u("Приглашение больше не действительно"));
                return;
            }

            const FamilyService::Result result =
                m_familyService.joinFamily(*invited, invitedSession->accountId, familyId);
            if (result != FamilyService::Result::Ok)
            {
                invited->sendClientMessage(ERROR_COLOUR, u(resultError(result)));
                return;
            }
            invited->sendClientMessage(INFO_COLOUR,
                                       u(fmt::format("Вы вступили в семью «{}». Чат семьи — команда /f", family->name)));
            // Уведомить онлайн-членов семьи о пополнении.
            const std::string note = u(fmt::format("[{}] {} вступил(а) в семью", family->name,
                                                   invited->getName().to_string()));
            for (IPlayer *member : m_core.getPlayers().entries())
            {
                if (member->getID() != invitedId && m_familyService.getFamilyId(member->getID()) == familyId)
                    member->sendClientMessage(INFO_COLOUR, note);
            }
        });
}

void FamilySystem::showLeaveConfirm(IPlayer &player)
{
    const int playerId = player.getID();
    const FamilyService::Family *family = m_familyService.getFamily(m_familyService.getFamilyId(playerId));
    if (!family)
    {
        showMenu(player);
        return;
    }

    std::string body = fmt::format("Выйти из семьи «{}»?", family->name);
    if (m_familyService.isOwner(playerId))
        body += "\nВы старейшина: владение перейдёт к старейшему из оставшихся, а если вы последний — семья будет "
                "распущена.";

    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u("Выход из семьи");
    dialog.body = u(body);
    dialog.leftButton = u("Выйти");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId](DialogResponse response, int, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showMenu(*player);
                return;
            }
            const PlayerSessionService::Session *session = m_sessionService.get(playerId);
            if (!session)
                return;
            const int familyId = m_familyService.getFamilyId(playerId);
            const FamilyService::Family *family = m_familyService.getFamily(familyId);
            if (!family)
            {
                showMenu(*player);
                return;
            }
            const std::string familyName = family->name; // копия до возможного роспуска
            const bool disbanded = m_familyService.leaveFamily(*player, session->accountId);
            player->sendClientMessage(INFO_COLOUR, u(fmt::format("Вы вышли из семьи «{}»", familyName)));
            if (!disbanded)
            {
                // Семья жива — уведомить оставшихся онлайн-членов (новый владелец
                // мог смениться; берём актуальное состояние).
                const std::string note = u(fmt::format("[{}] Участник покинул семью", familyName));
                for (IPlayer *member : m_core.getPlayers().entries())
                {
                    if (m_familyService.getFamilyId(member->getID()) == familyId)
                        member->sendClientMessage(INFO_COLOUR, note);
                }
            }
        });
}

void FamilySystem::showDisbandConfirm(IPlayer &player)
{
    if (!m_familyService.isOwner(player.getID()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Это может только владелец семьи"));
        return;
    }
    const FamilyService::Family *family = m_familyService.getFamily(m_familyService.getFamilyId(player.getID()));
    if (!family)
        return;

    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u("Роспуск семьи");
    dialog.body = u(fmt::format("Распустить семью «{}»? Это действие нельзя отменить.", family->name));
    dialog.leftButton = u("Распустить");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID()](DialogResponse response, int, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showMenu(*player);
                return;
            }
            // Перепроверка владельца на момент клика.
            if (!m_familyService.isOwner(playerId))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Это может только владелец семьи"));
                return;
            }
            const int familyId = m_familyService.getFamilyId(playerId);
            const FamilyService::Family *family = m_familyService.getFamily(familyId);
            const std::string familyName = family ? family->name : "";
            // Снимок онлайн-членов ДО роспуска — после него слоты обнулятся.
            std::vector<int> onlineMembers;
            for (IPlayer *member : m_core.getPlayers().entries())
            {
                if (m_familyService.getFamilyId(member->getID()) == familyId)
                    onlineMembers.push_back(member->getID());
            }

            if (!m_familyService.disbandFamily(playerId))
                return;

            const std::string note = u(fmt::format("Семья «{}» распущена", familyName));
            for (const int memberId : onlineMembers)
            {
                if (IPlayer *member = m_core.getPlayers().get(memberId))
                    member->sendClientMessage(INFO_COLOUR, note);
            }
        });
}
