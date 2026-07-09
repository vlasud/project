#include "Systems/FamilySystem/FamilySystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/ParkedVehicleService/ParkedVehicleRow.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <chrono>
#include <ctime>
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

// Клиентские цветокоды ячейки TABLIST «Статус» в «Члены семьи» — тот же зелёный,
// что у HouseSystem (свободный дом, 90EE90), и hex ERROR_COLOUR (FF5A5A) для
// оффлайна: единая палитра проекта, не новые оттенки.
constexpr const char *ONLINE_TAG = "{90EE90}онлайн";
constexpr const char *OFFLINE_TAG = "{FF5A5A}оффлайн";

// Праздничный попап на создание семьи (ScreenNoticeService): зелёный — позитив
// (тот же 90EE90, что и выше), 5с — как приветствие при входе (некритичный
// текст, времени заметить достаточно).
constexpr Milliseconds FAMILY_CREATED_POPUP_TIME{5000};
const Colour FAMILY_CREATED_POPUP_COLOUR{0x90, 0xEE, 0x90, 0xFF};

// Текст ошибки операции членства для клиента (utf-8).
const char *resultError(FamilyService::Result result)
{
    switch (result)
    {
    case FamilyService::Result::InvalidName:
        return "Название — одно слово латиницей (3-24 буквы), без цифр, пробелов и символов";
    case FamilyService::Result::NameTaken:
        return "Семья с таким названием уже существует (регистр не различается)";
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
    case FamilyService::Result::NotOwner:
        return "Это может только лидер семьи";
    case FamilyService::Result::TargetNotFound:
        return "Этот игрок больше не в семье";
    case FamilyService::Result::CannotKickSelf:
        return "Себя выгнать нельзя — используйте «Покинуть семью»";
    case FamilyService::Result::Ok:
        return "";
    }
    return "";
}

// Дата "d.m.Y" из unix-времени (нет прецедента формата дат в проекте — берём
// простой календарный формат). gmtime — без TZ-обвязки, для UI-даты достаточно.
std::string formatDate(long long unixTime)
{
    const std::time_t time = static_cast<std::time_t>(unixTime);
    if (const std::tm *tm = std::gmtime(&time))
        return fmt::format("{:02}.{:02}.{}", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);
    return "?";
}
} // namespace

FamilySystem::FamilySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_familyService(serviceRegister.getService<FamilyService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_chatService(serviceRegister.getService<PlayerChatService>()),
      m_parkedService(serviceRegister.getService<ParkedVehicleService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_screenNotice(serviceRegister.getService<ScreenNoticeService>())
{
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            m_familyService.onSessionStart(player.getID(), session.accountId);
        });
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            m_familyService.onSessionEnd(player.getID());
        });

    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add(
        "family", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            showMenu(player);
        },
        {}, "меню семьи: информация, состав, машины, приглашение, выход", PlayerCommandService::HelpCategory::Misc);

    commands.add(
        "f", {{PlayerCommandService::Param::String, "текст"}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            familyChat(player, args.getString(0));
        },
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
                {
                    LogManager::log(Error, "FamilySystem: failed to load family members: " + error);
                });
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "FamilySystem: failed to load families: " + error);
        });
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
    // Правило видимости: ВСЕ 6 пунктов видны ВСЕГДА любому члену — недоступность
    // (не владелец) объясняет обработчик сообщением при клике, не скрытием.
    actions.push_back(Action::Info);
    actions.push_back(Action::Members);
    actions.push_back(Action::Vehicles);
    actions.push_back(Action::Invite);
    actions.push_back(Action::Leave);
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
        case Action::Info:
            body += "Информация\n";
            break;
        case Action::Members:
            body += "Члены семьи\n";
            break;
        case Action::Vehicles:
            body += "Транспорт семьи\n";
            break;
        case Action::Invite:
            body += "Пригласить в семью\n";
            break;
        case Action::Leave:
            body += "Покинуть семью\n";
            break;
        }
    }
    if (!body.empty())
        body.pop_back();

    Dialog dialog =
        makeDialog(DialogStyle_LIST, family ? fmt::format("Семья «{}»", family->name) : std::string("Семья"), body,
                   "Выбрать", "Закрыть");

    m_dialogService.show(player, dialog,
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
                             // Действие должно остаться доступным (напр. семью распустили).
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
                             case Action::Info:
                                 showInfo(*player);
                                 break;
                             case Action::Members:
                                 showMembers(*player);
                                 break;
                             case Action::Vehicles:
                                 showVehicles(*player);
                                 break;
                             case Action::Invite:
                                 showInviteInput(*player);
                                 break;
                             case Action::Leave:
                                 showLeaveConfirm(*player);
                                 break;
                             }
                         });
}

void FamilySystem::showCreateInput(IPlayer &player)
{
    Dialog dialog = makeDialog(DialogStyle_INPUT, "Создание семьи",
                               fmt::format("Введите название семьи: одно слово латиницей, {}-{} букв\n"
                                           "(без цифр, пробелов и символов). Создание бесплатно.",
                                           FamilyService::NAME_MIN, FamilyService::NAME_MAX),
                               "Создать", "Назад");

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
            const FamilyService::Family *family = m_familyService.getFamily(m_familyService.getFamilyId(playerId));
            player->sendClientMessage(
                INFO_COLOUR, u(fmt::format("Семья «{}» создана. Вы — лидер. Управление семьей — команда /family",
                                           family ? family->name : "")));
            // Праздничный попап поверх чата: тексты попапов — английские (как
            // engine is broken / no fuel), зелёный — позитив из палитры проекта.
            m_screenNotice.show(*player, "congratulations!", FAMILY_CREATED_POPUP_TIME, FAMILY_CREATED_POPUP_COLOUR);
        });
}

// ------------------------------------------------------------------ 1: информация

void FamilySystem::showInfo(IPlayer &player)
{
    const int playerId = player.getID();
    const FamilyService::Family *family = m_familyService.getFamily(m_familyService.getFamilyId(playerId));
    if (!family)
    {
        showMenu(player);
        return;
    }

    // Имя владельца — из состава (он всегда член); онлайн -> «Имя[id]» актуальным ником.
    std::string ownerName = "?";
    for (const FamilyService::Mem &member : family->members)
    {
        if (member.accountId == family->ownerAccountId)
        {
            // Снимок из БД — оборонительная чистка цветокодов, как в «Члены семьи».
            ownerName = Encoding::neutralizeColorCodes(member.name);
            break;
        }
    }
    const int ownerPlayerId = m_sessionService.playerByAccount(family->ownerAccountId);
    if (IPlayer *ownerPlayer = ownerPlayerId != -1 ? m_core.getPlayers().get(ownerPlayerId) : nullptr)
        ownerName = fmt::format("{}[{}]", ownerPlayer->getName().to_string(), ownerPlayerId);

    // TABLIST_HEADERS «Поле | Значение» (образец — HomeMenuSystem::showInfo).
    // Раздел расширяемый — новые факты о семье добавляются новыми строками.
    std::string body = "Поле\tЗначение\n";
    body += fmt::format("Название\t{}\n", family->name);
    body += fmt::format("Лидер\t{}\n", ownerName);
    body += fmt::format("Членов\t{}/{}\n", family->members.size(), FamilyService::MAX_MEMBERS);
    body += fmt::format("Создана\t{}", formatDate(family->createdAt));

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, "Информация о семье", body, "Назад", ""),
                         [this, playerId](DialogResponse, int, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (player)
                                 showMenu(*player);
                         });
}

// ------------------------------------------------------------------ 2: члены семьи

void FamilySystem::showMembers(IPlayer &player)
{
    const int playerId = player.getID();
    const FamilyService::Family *family = m_familyService.getFamily(m_familyService.getFamilyId(playerId));
    if (!family)
    {
        showMenu(player);
        return;
    }

    const PlayerSessionService::Session *mySession = m_sessionService.get(playerId);
    const FamilyService::AccountId myAccount = mySession ? mySession->accountId : PlayerSessionService::NO_ACCOUNT;

    // Онлайн-сортировка: стабильный partition онлайн/оффлайн поверх members (уже
    // отсортированы по joinedAt по возрастанию) — внутри каждой группы стаж
    // сохраняется, т.к. partition стабилен.
    std::vector<const FamilyService::Mem *> ordered;
    ordered.reserve(family->members.size());
    for (const FamilyService::Mem &member : family->members)
        ordered.push_back(&member);
    std::stable_partition(ordered.begin(), ordered.end(),
                          [this](const FamilyService::Mem *m)
                          {
                              return m_sessionService.playerByAccount(m->accountId) != -1;
                          });

    std::string body = "Игрок\tСтатус\n";
    // Снимок ПОРЯДКА на момент показа: listItem клика резолвится по нему, а не по
    // живому составу — онлайн-сортировка могла перетасовать список, пока диалог
    // висел (кто-то вошёл/вышел), и живой индекс указал бы на другого человека.
    std::vector<FamilyService::AccountId> order;
    order.reserve(ordered.size());
    for (const FamilyService::Mem *member : ordered)
    {
        order.push_back(member->accountId);
        const int memberPlayerId = m_sessionService.playerByAccount(member->accountId);
        IPlayer *memberPlayer = memberPlayerId != -1 ? m_core.getPlayers().get(memberPlayerId) : nullptr;
        // Онлайн — АКТУАЛЬНЫЙ ник (getName, игрок мог переименоваться); оффлайн —
        // снимок Mem::name на момент вступления (utf-8 -> u() ниже вместе со всем телом).
        // Снимок из БД чистится оборонительно: цветокоды {}/~ раскрашивает клиент, а
        // инвариант «ник = латиница» для legacy/правленных вручную строк не гарантирован.
        std::string nameField = memberPlayer
                                    ? fmt::format("{}[{}]", memberPlayer->getName().to_string(), memberPlayerId)
                                    : Encoding::neutralizeColorCodes(member->name);
        // Один суффикс вместо двух подряд скобок — короче для tablist-ячейки.
        const bool isOwnerRow = member->accountId == family->ownerAccountId;
        const bool isSelf = myAccount != PlayerSessionService::NO_ACCOUNT && member->accountId == myAccount;
        if (isOwnerRow && isSelf)
            nameField += " (вы, лидер)";
        else if (isOwnerRow)
            nameField += " (лидер)";
        else if (isSelf)
            nameField += " (вы)";
        body += fmt::format("{}\t{}\n", nameField, memberPlayer ? ONLINE_TAG : OFFLINE_TAG);
    }
    if (!body.empty())
        body.pop_back();

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, "Члены семьи", body, "Выбрать", "Назад"),
                         [this, playerId, order](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response != DialogResponse_Left)
                             {
                                 showMenu(*player);
                                 return;
                             }
                             if (listItem < 0 || static_cast<std::size_t>(listItem) >= order.size())
                             {
                                 showMembers(*player); // мусорный индекс — перепоказать
                                 return;
                             }
                             showMemberActions(*player, order[listItem]);
                         });
}

// Член семьи для подменю/карточки: актуальная запись состава по accountId (снимок
// порядка мог устареть — членство ре-валидируется здесь на каждом входе).
const FamilyService::Mem *FamilySystem::memberByAccount(int playerId, FamilyService::AccountId targetAccount) const
{
    const FamilyService::Family *family = m_familyService.getFamily(m_familyService.getFamilyId(playerId));
    if (!family)
        return nullptr;
    for (const FamilyService::Mem &member : family->members)
    {
        if (member.accountId == targetAccount)
            return &member;
    }
    return nullptr;
}

// Отображаемое имя члена: онлайн — актуальный «Ник[id]», оффлайн — чищенный снимок.
std::string FamilySystem::memberDisplayName(FamilyService::AccountId targetAccount,
                                            const FamilyService::Mem &member) const
{
    const int targetPlayerId = m_sessionService.playerByAccount(targetAccount);
    if (IPlayer *target = targetPlayerId != -1 ? m_core.getPlayers().get(targetPlayerId) : nullptr)
        return fmt::format("{}[{}]", target->getName().to_string(), targetPlayerId);
    return Encoding::neutralizeColorCodes(member.name);
}

void FamilySystem::showMemberActions(IPlayer &player, FamilyService::AccountId targetAccount)
{
    const int playerId = player.getID();
    const FamilyService::Mem *member = memberByAccount(playerId, targetAccount);
    if (!member)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот игрок больше не в семье"));
        showMembers(player);
        return;
    }

    // Оба пункта видны ВСЕМ (правило видимости): «Исключить» рядовому объяснит
    // отказ обработчик, не скрытие.
    m_dialogService.show(player,
                         makeDialog(DialogStyle_LIST, memberDisplayName(targetAccount, *member),
                                    "Информация\nИсключить", "Выбрать", "Назад"),
                         [this, playerId, targetAccount](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response != DialogResponse_Left)
                             {
                                 showMembers(*player);
                                 return;
                             }
                             // Ре-валидация членства цели на КАЖДОМ клике (могли кикнуть/выйти).
                             const FamilyService::Mem *member = memberByAccount(playerId, targetAccount);
                             if (!member)
                             {
                                 player->sendClientMessage(ERROR_COLOUR, u("Этот игрок больше не в семье"));
                                 showMembers(*player);
                                 return;
                             }
                             if (listItem == 0)
                             {
                                 showMemberInfo(*player, targetAccount);
                             }
                             else if (listItem == 1)
                             {
                                 // Гейты кика: лидер, не сам себя; снимок targetAccount уже в руках.
                                 if (!m_familyService.isOwner(playerId))
                                 {
                                     player->sendClientMessage(ERROR_COLOUR, u("Это может только лидер семьи"));
                                     showMemberActions(*player, targetAccount);
                                     return;
                                 }
                                 const PlayerSessionService::Session *session = m_sessionService.get(playerId);
                                 if (session && session->accountId == targetAccount)
                                 {
                                     player->sendClientMessage(ERROR_COLOUR,
                                                               u(resultError(FamilyService::Result::CannotKickSelf)));
                                     showMemberActions(*player, targetAccount);
                                     return;
                                 }
                                 showKickConfirm(*player, targetAccount, memberDisplayName(targetAccount, *member));
                             }
                         });
}

void FamilySystem::showMemberInfo(IPlayer &player, FamilyService::AccountId targetAccount)
{
    const int playerId = player.getID();
    const FamilyService::Mem *member = memberByAccount(playerId, targetAccount);
    if (!member)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот игрок больше не в семье"));
        showMembers(player);
        return;
    }

    // Базовая карточка (раздел расширяемый — новые факты добавляются строками).
    const int targetPlayerId = m_sessionService.playerByAccount(targetAccount);
    std::string body = "Поле\tЗначение\n";
    body += fmt::format("Игрок\t{}\n", memberDisplayName(targetAccount, *member));
    body += fmt::format("Статус\t{}\n", targetPlayerId != -1 ? ONLINE_TAG : OFFLINE_TAG);
    body += fmt::format("В семье с\t{}", formatDate(member->joinedAt));

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, "Информация об игроке", body, "Назад", ""),
                         [this, playerId, targetAccount](DialogResponse, int, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (player)
                                 showMemberActions(*player, targetAccount);
                         });
}

// ------------------------------------------------------------------ 3: транспорт семьи

void FamilySystem::showVehicles(IPlayer &player)
{
    const int playerId = player.getID();
    const int familyId = m_familyService.getFamilyId(playerId);
    if (familyId == FamilyService::NO_FAMILY)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы не состоите в семье"));
        return;
    }

    const std::vector<long long> dbIds = m_parkedService.parkedOfFamily(familyId);
    if (dbIds.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("В семье пока нет расшаренных машин"));
        return;
    }

    // Формат /car «Мои машины» (TABLIST_HEADERS «Машина | Где находится | Топливо»,
    // общий builder — см. ParkedVehicleRow.h), но READ-ONLY: выбор строки ничего не
    // делает. «Забрать» доступа семьи — только у владельца МАШИНЫ через /car ->
    // «Вернуть от семьи» (см. Docs/ParkedVehicles.md).
    std::string body = "Машина\tГде находится\tТопливо\n";
    for (const long long dbId : dbIds)
    {
        const ParkedVehicleService::Parked *parked = m_parkedService.byDbId(dbId);
        const int model = parked ? parked->model : 0;
        const int liveId = parked ? parked->vehicleId : -1;
        body +=
            ParkedVehicleRow::build(m_parkedService, m_vehicleService, m_core.getPlayers(), dbId, liveId, model) + "\n";
    }
    if (!body.empty())
        body.pop_back();

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, "Транспорт семьи", body, "Назад", ""),
                         [this, playerId](DialogResponse, int, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (player)
                                 showMenu(*player);
                         });
}

// ------------------------------------------------------------------ 4: пригласить по нику

void FamilySystem::showInviteInput(IPlayer &player)
{
    if (!m_familyService.isOwner(player.getID()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Это может только лидер семьи"));
        return;
    }

    Dialog dialog = makeDialog(DialogStyle_INPUT, "Приглашение в семью",
                               "Введите id игрока (посмотреть можно по TAB), которого хотите пригласить.\n"
                               "Он должен быть в сети и не состоять в семье.",
                               "Пригласить", "Назад");

    m_dialogService.showNumberInput(
        player, dialog,
        [this, ownerId = player.getID()](DialogResponse response, std::int64_t value)
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
                owner->sendClientMessage(ERROR_COLOUR, u("Это может только лидер семьи"));
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

            // Приглашение по СЕРВЕРНОМУ id (как передача дома в /home): мусор/вне
            // диапазона — ошибка ВВОДА, не «нет такого игрока» (разные тексты).
            if (value < 0 || value >= MAX_PLAYERS)
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Введите корректный id игрока"));
                showInviteInput(*owner);
                return;
            }
            IPlayer *target = m_core.getPlayers().get(static_cast<int>(value));
            if (!target)
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Такого игрока нет в сети"));
                showInviteInput(*owner);
                return;
            }
            const int targetId = target->getID();
            if (targetId == ownerId)
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Нельзя пригласить самого себя"));
                showInviteInput(*owner);
                return;
            }
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            if (!targetSession)
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

            owner->sendClientMessage(INFO_COLOUR,
                                     u(fmt::format("Приглашение отправлено игроку {}", target->getName().to_string())));
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

    Dialog dialog = makeDialog(DialogStyle_MSGBOX, "Приглашение в семью",
                               fmt::format("{} приглашает вас в семью «{}». Принять приглашение?",
                                           inviter->getName().to_string(), family->name),
                               "Принять", "Отклонить");

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
            invited->sendClientMessage(
                INFO_COLOUR, u(fmt::format("Вы вступили в семью «{}». Чат семьи — команда /f", family->name)));
            // Уведомить онлайн-членов семьи о пополнении.
            const std::string note =
                u(fmt::format("[{}] {} вступил(а) в семью", family->name, invited->getName().to_string()));
            for (IPlayer *member : m_core.getPlayers().entries())
            {
                if (member->getID() != invitedId && m_familyService.getFamilyId(member->getID()) == familyId)
                    member->sendClientMessage(INFO_COLOUR, note);
            }
        });
}

// -------------------------------------------------- исключение (из подменю члена)

void FamilySystem::showKickConfirm(IPlayer &owner, FamilyService::AccountId targetAccount,
                                   const std::string &targetName)
{
    Dialog dialog = makeDialog(DialogStyle_MSGBOX, "Исключение из семьи",
                               fmt::format("Выгнать {} из семьи?", targetName), "Выгнать", "Назад");

    // Снимок targetAccount (НЕ ник) захватывает колбэк — переименование/перелогин
    // цели между показом и «Да» не уводит кик не туда (паттерн /home expectedAccount).
    m_dialogService.show(
        owner, dialog,
        [this, ownerId = owner.getID(), targetAccount, targetName](DialogResponse response, int, StringView)
        {
            IPlayer *owner = m_core.getPlayers().get(ownerId);
            if (!owner)
                return;
            if (response != DialogResponse_Left)
            {
                showMenu(*owner);
                return;
            }
            const int familyId = m_familyService.getFamilyId(ownerId);
            const FamilyService::Family *family = m_familyService.getFamily(familyId);
            const std::string familyNameCopy = family ? family->name : std::string("?");

            const FamilyService::Result result = m_familyService.kickMember(ownerId, targetAccount);
            if (result != FamilyService::Result::Ok)
            {
                owner->sendClientMessage(ERROR_COLOUR, u(resultError(result)));
                return;
            }

            // Расшаренные семье машины ВЫГНАННОГО (он мог быть владельцем машин) —
            // снять шеринг тем же путём, что при обычном выходе владельца машин.
            m_parkedService.onOwnerLeftFamily(targetAccount);

            owner->sendClientMessage(INFO_COLOUR, u(fmt::format("{} исключён(а) из семьи", targetName)));

            const int targetPlayerId = m_sessionService.playerByAccount(targetAccount);
            if (IPlayer *targetPlayer = targetPlayerId != -1 ? m_core.getPlayers().get(targetPlayerId) : nullptr)
                targetPlayer->sendClientMessage(INFO_COLOUR,
                                                u(fmt::format("Вас исключили из семьи «{}»", familyNameCopy)));

            const std::string note = u(fmt::format("[{}] {} исключён(а) из семьи", familyNameCopy, targetName));
            for (IPlayer *member : m_core.getPlayers().entries())
            {
                if (member->getID() != ownerId && member->getID() != targetPlayerId &&
                    m_familyService.getFamilyId(member->getID()) == familyId)
                    member->sendClientMessage(INFO_COLOUR, note);
            }
        });
}

// ------------------------------------------------------------------ 6: покинуть семью

void FamilySystem::showLeaveConfirm(IPlayer &player)
{
    const int playerId = player.getID();
    const FamilyService::Family *family = m_familyService.getFamily(m_familyService.getFamilyId(playerId));
    if (!family)
    {
        showMenu(player);
        return;
    }
    const bool owner = m_familyService.isOwner(playerId);

    // Семантика различается: рядовой — простой выход; владелец — «Покинуть» это
    // ПОЛНЫЙ РОСПУСК (наследование власти из UI убрано, см. Docs/Family.md).
    // Перенос строкой отделяет «что произойдёт» от «нельзя отменить» — вёрстка
    // деструктивных подтверждений как у передачи дома (/home).
    const std::string body =
        owner ? fmt::format("Покинуть семью? Вы лидер — семья «{}» будет РАСПУЩЕНА полностью.\nЭто действие "
                            "нельзя отменить.",
                            family->name)
              : fmt::format("Покинуть семью «{}»?", family->name);

    Dialog dialog = makeDialog(DialogStyle_MSGBOX, "Покинуть семью", body, "Покинуть", "Назад");

    m_dialogService.show(player, dialog,
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
                             // Ре-валидация владения на момент клика (диалог мог висеть, пока
                             // владельца сменили/выгнали).
                             if (m_familyService.isOwner(playerId))
                             {
                                 const std::string familyName = family->name;
                                 std::vector<int> onlineMembers;
                                 for (IPlayer *member : m_core.getPlayers().entries())
                                 {
                                     if (m_familyService.getFamilyId(member->getID()) == familyId)
                                         onlineMembers.push_back(member->getID());
                                 }
                                 if (!m_familyService.disbandFamily(playerId))
                                     return;
                                 // Роспуск: снять шеринг у всех машин семьи. Машины остаются
                                 // припаркованы ЛИЧНО у дома владельцев (экземпляры не уничтожаются).
                                 m_parkedService.onFamilyDissolved(familyId);
                                 const std::string note = u(fmt::format("Семья «{}» распущена", familyName));
                                 for (const int memberId : onlineMembers)
                                 {
                                     if (IPlayer *member = m_core.getPlayers().get(memberId))
                                         member->sendClientMessage(INFO_COLOUR, note);
                                 }
                                 return;
                             }

                             // Рядовой член: простой выход.
                             const std::string familyName = family->name; // копия до возможного роспуска (страховка)
                             const FamilyService::AccountId accountId = session->accountId;
                             const bool disbanded = m_familyService.leaveFamily(*player, session->accountId);
                             // Крайний случай: последний рядовой участник семьи БЕЗ владельца (не
                             // должен встречаться из этой ветки — владелец уходит через disband
                             // выше) — снять шеринг на всякий случай тем же путём, что и раньше.
                             if (disbanded)
                                 m_parkedService.onFamilyDissolved(familyId);
                             else
                                 m_parkedService.onOwnerLeftFamily(accountId);
                             player->sendClientMessage(INFO_COLOUR,
                                                       u(fmt::format("Вы вышли из семьи «{}»", familyName)));
                             if (!disbanded)
                             {
                                 const std::string note = u(fmt::format("[{}] Участник покинул семью", familyName));
                                 for (IPlayer *member : m_core.getPlayers().entries())
                                 {
                                     if (m_familyService.getFamilyId(member->getID()) == familyId)
                                         member->sendClientMessage(INFO_COLOUR, note);
                                 }
                             }
                         });
}
