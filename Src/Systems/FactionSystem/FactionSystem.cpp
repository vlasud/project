#include "Systems/FactionSystem/FactionSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};
// Цвет гражданина (ник/маркер вне фракции). Источник правды о «гражданском»
// цвете пока здесь; цветовая система игрока — будущая задача.
const Colour CIVILIAN_COLOUR{200, 200, 200};
// Рация организации: весь текст светло-зелёный.
const Colour RADIO_COLOUR{144, 238, 144};
constexpr std::size_t MAX_RADIO_BYTES = 180; // utf-8, ~90 кириллических

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

FactionSystem::FactionSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_bankService(serviceRegister.getService<BankService>()),
      m_pickupService(serviceRegister.getService<PickupService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_spawnService(serviceRegister.getService<PlayerSpawnService>()),
      m_skinService(serviceRegister.getService<PlayerSkinService>()),
      m_personalSkinService(serviceRegister.getService<PlayerPersonalSkinService>())
{
    // Спавн, цвет и скин члена — от его организации (вступление/выход/появление
    // в сети применяют немедленно; спавн действует на все последующие спавны).
    m_factionService.subscribeMemberChange(
        [this](IPlayer &player, int oldFactionId, int newFactionId)
        {
            applyFactionSpawn(player, newFactionId);
            applyFactionColour(player, newFactionId);
            applyFactionSkin(player, oldFactionId, newFactionId);
        });

    // Временно здесь: когда появятся системы остальных конкретных фракций,
    // каждая зарегистрирует свою в собственном конструкторе (администрация —
    // PresidentAdministrationSystem, образец).
    // Гос-вертикаль: фракция 1 — администрация, её лидер — ПРЕЗИДЕНТ
    // (назначается итогом выборов автоматически). Президент создаёт ранги
    // («Министр ВД», ...) и в меню ранга передаёт каждому в управление
    // КОНКРЕТНЫЕ подопечные организации — их лидеров ранг назначает через
    // /gov. Банки городов — тоже фракции; деньги организаций лежат «в банке»
    // (бюджет), зарплатные чеки уходят на счета BankService. Фракция-банк (id 5)
    // переехала в собственную систему BankFactionSystem.
    m_factionService.registerFaction(6, "Банк Сан-Фиерро", 1);
    m_factionService.registerColour(6, Colour(60, 179, 113));
    m_factionService.registerFaction(7, "Банк Лас-Вентураса", 1);
    m_factionService.registerColour(7, Colour(32, 178, 170));

    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            loadMembership(player, session);
        });
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            m_factionService.resetPlayer(player.getID());
        });

    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add("faction", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     if (m_factionService.isLeader(player.getID()))
                         showLeaderMenu(player);
                     else
                         showFactionInfo(player);
                 },
                 {}, "меню организации: состав, ранги, бюджет", PlayerCommandService::HelpCategory::Faction);

    // --- команды лидера по людям ---

    commands.add("invite", {{PlayerCommandService::Param::Int, "id игрока"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     inviteMember(player, args.getInt(0));
                 },
                 {}, "пригласить игрока в организацию", PlayerCommandService::HelpCategory::Faction);

    commands.add("uninvite", {{PlayerCommandService::Param::Int, "id игрока"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     uninviteMember(player, args.getInt(0));
                 },
                 {}, "уволить игрока из организации", PlayerCommandService::HelpCategory::Faction);

    commands.add("setrank", {{PlayerCommandService::Param::Int, "id игрока"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     showSetRankDialog(player, args.getInt(0));
                 },
                 {}, "назначить игроку ранг в организации", PlayerCommandService::HelpCategory::Faction);

    commands.add("setsalary", {{PlayerCommandService::Param::Int, "id игрока"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     showSetSalaryDialog(player, args.getInt(0));
                 },
                 {}, "назначить игроку зарплату", PlayerCommandService::HelpCategory::Faction);

    // Рация организации: сообщение всем членам.
    commands.add("r", {{PlayerCommandService::Param::String, "текст"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     radioChat(player, args.getString(0));
                 },
                 {}, "рация: написать всем в своей организации", PlayerCommandService::HelpCategory::Faction);

    // Смена скина из пула организации (право «Смена скина»).
    commands.add("skin", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showSkinDialog(player); },
                 {}, "сменить скин из пула организации", PlayerCommandService::HelpCategory::Faction);

    // Куратор (министр/президент): управление лидерами подопечных фракций.
    commands.add("gov", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     showGovMenu(player);
                 },
                 {}, "управление лидерами подопечных фракций", PlayerCommandService::HelpCategory::Faction);

    // Единое дев-меню фракций; доступ только разработчику (admin level 6).
    commands.add("fdev", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     showDevMenu(player);
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "дев-меню фракций (меню)",
                 PlayerCommandService::HelpCategory::Hidden);
}

void FactionSystem::initialize(IComponentList *components)
{
    loadCatalog();
    createBasePickups();
}

// ------------------------------------------------------------------ базы организаций

void FactionSystem::createBasePickups()
{
    for (const FactionService::Faction &faction : m_factionService.getFactions())
    {
        if (!faction.base.defined)
            continue;
        const int factionId = faction.id;
        for (const FactionService::BaseDoor &door : faction.base.entrances)
        {
            m_pickupService.add(faction.base.pickupModel, 1, door.pickupPos,
                                [this, factionId, target = door.targetPos, angle = door.targetAngle](IPlayer &player)
                                { enterBase(player, factionId, target, angle); });
        }
        // Пикапы выходов живут в мире базы (= id фракции).
        for (const FactionService::BaseDoor &door : faction.base.exits)
        {
            m_pickupService.add(faction.base.pickupModel, 1, door.pickupPos,
                                [this, factionId, target = door.targetPos, angle = door.targetAngle](IPlayer &player)
                                { exitBase(player, factionId, target, angle); },
                                static_cast<std::uint32_t>(factionId));
        }
    }
}

void FactionSystem::enterBase(IPlayer &player, int factionId, const Vector3 &target, float angle)
{
    const FactionService::Faction *faction = m_factionService.getFaction(factionId);
    if (!faction || !faction->base.defined)
        return;
    if (m_factionService.getMemberFaction(player.getID()) != factionId)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Вход только для сотрудников «{}»", faction->name)));
        return;
    }

    m_locationService.teleport(player, target, static_cast<unsigned>(faction->base.interior), factionId);
    player.setRotation(GTAQuat(Vector3(0.0f, 0.0f, angle)));
    player.setCameraBehind();
}

void FactionSystem::exitBase(IPlayer &player, int factionId, const Vector3 &target, float angle)
{
    const FactionService::Faction *faction = m_factionService.getFaction(factionId);
    if (!faction || !faction->base.defined)
        return;

    m_locationService.teleport(player, target, 0, 0);
    player.setRotation(GTAQuat(Vector3(0.0f, 0.0f, angle)));
    player.setCameraBehind();
}

void FactionSystem::applyFactionColour(IPlayer &player, int factionId)
{
    const FactionService::Faction *faction = m_factionService.getFaction(factionId);
    // setColour красит и ник, и маркер на миникарте.
    player.setColour(faction ? faction->colour : CIVILIAN_COLOUR);
}

void FactionSystem::applyFactionSkin(IPlayer &player, int oldFactionId, int newFactionId)
{
    const int playerId = player.getID();

    if (newFactionId != FactionService::NO_FACTION)
    {
        // Вход/появление в сети члена. Скин организации обязателен, пока состоит.
        // ОТОБРАЖАЕТСЯ органный скин, но ЛИЧНЫЙ (гражданский) скин аккаунта в
        // PlayerPersonalSkinService остаётся неизменным — его и вернём при
        // увольнении. На входе его трогать не нужно: auth применил личный скин
        // ДО старта членства, и здесь он уже лежит в источнике правды о личном.
        const int orgSkin = m_factionService.resolveOrgSkin(newFactionId, m_factionService.getMemberSkin(playerId));
        if (orgSkin < 0)
            return; // у организации нет пула (напр. банк) — применять нечего

        m_skinService.setSkin(player, orgSkin);
        return;
    }

    // Выход из фракции — возврат ЛИЧНОГО скина аккаунта (источник правды —
    // PlayerPersonalSkinService; всегда валиден, фолбэк на дефолт внутри него).
    m_skinService.setSkin(player, m_personalSkinService.getSkin(playerId));
}

void FactionSystem::radioChat(IPlayer &player, StringView rawText)
{
    const int playerId = player.getID();
    const int factionId = m_factionService.getMemberFaction(playerId);
    const FactionService::Faction *faction = m_factionService.getFaction(factionId);
    if (!faction)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы не состоите в организации"));
        return;
    }

    // Ввод клиента: cp1251 -> utf-8, чистка и обрезка без разрыва символа.
    const std::string text = Encoding::sanitizeUserText(
        Encoding::cp1251Toutf8(std::string(rawText.data(), rawText.size())), MAX_RADIO_BYTES);
    if (text.empty())
        return;

    const FactionService::Rank *rank = m_factionService.getMemberRank(playerId);
    const std::string message = u(fmt::format("[R] [{}] {}[{}] : {}", rank ? rank->name : "?",
                                              player.getName().to_string(), playerId, text));

    for (IPlayer *member : m_core.getPlayers().entries())
    {
        if (m_factionService.getMemberFaction(member->getID()) == factionId)
            member->sendClientMessage(RADIO_COLOUR, message);
    }
}

void FactionSystem::showSkinDialog(IPlayer &player)
{
    const FactionService::Faction *faction = permittedFaction(player, FactionService::PERM_SKIN);
    if (!faction)
        return;
    if (faction->skins.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("У организации нет пула скинов"));
        return;
    }

    std::string body;
    for (const int skin : faction->skins)
        body += fmt::format("Скин {}\n", skin);
    body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(fmt::format("{} — скины", faction->name));
    dialog.body = u(body);
    dialog.leftButton = u("Надеть");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;
            const FactionService::Faction *faction =
                m_factionService.getFaction(m_factionService.getMemberFaction(playerId));
            if (!faction || listItem < 0 || static_cast<std::size_t>(listItem) >= faction->skins.size())
                return;

            const int skin = faction->skins[listItem];
            // Перепроверка права и принадлежности скина пулу — состояние могло
            // измениться, пока диалог был открыт.
            if (!m_factionService.canUseSkin(playerId, skin))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Недостаточно прав во фракции"));
                return;
            }
            // Выбор сохраняется в БД (write-through) — на следующем заходе
            // возьмётся из БД; setMemberSkin валидирует пул ещё раз.
            if (!m_factionService.setMemberSkin(*player, skin))
                return;
            m_skinService.setSkin(*player, skin);
            player->sendClientMessage(INFO_COLOUR, u(fmt::format("Скин сменён на {}", skin)));
        });
}

void FactionSystem::applyFactionSpawn(IPlayer &player, int factionId)
{
    const FactionService::Faction *faction = m_factionService.getFaction(factionId);
    if (faction && faction->spawn.defined)
    {
        SpawnPoint point;
        point.position = faction->spawn.position;
        point.angle = faction->spawn.angle;
        point.interior = static_cast<unsigned>(faction->spawn.interior);
        point.virtualWorld = faction->spawn.virtualWorld;
        m_spawnService.setSpawn(player, point);
        return;
    }
    // Вне фракции (или у неё нет точки) — гражданский дефолт.
    m_spawnService.setSpawn(player, SpawnPoint{});
}

// ------------------------------------------------------------------ загрузка

void FactionSystem::loadCatalog()
{
    DatabaseManager::selectQuery<std::vector<std::pair<int, FactionService::Rank>>>(
        [](mysqlx::Schema schema)
        {
            mysqlx::RowResult rankRows = schema.getTable("faction_rank")
                                             .select("id", "faction_id", "name", "permissions", "is_default")
                                             .execute();
            std::vector<std::pair<int, FactionService::Rank>> ranks;
            while (mysqlx::Row row = rankRows.fetchOne())
            {
                FactionService::Rank rank;
                rank.id = row.get(0).get<std::int64_t>();
                rank.name = row.get(2).get<std::string>();
                rank.permissions = static_cast<FactionService::PermissionMask>(row.get(3).get<std::int64_t>());
                rank.isDefault = row.get(4).get<int>() != 0;
                ranks.emplace_back(row.get(1).get<int>(), std::move(rank));
            }
            return ranks;
        },
        [this](std::vector<std::pair<int, FactionService::Rank>> ranks)
        {
            m_factionService.loadRanks(std::move(ranks));

            // Скоупы подопечных организаций — строго после рангов.
            DatabaseManager::selectQuery<std::vector<std::pair<std::int64_t, int>>>(
                [](mysqlx::Schema schema)
                {
                    mysqlx::RowResult scopeRows =
                        schema.getTable("faction_rank_scope").select("rank_id", "faction_id").execute();
                    std::vector<std::pair<std::int64_t, int>> scopes;
                    while (mysqlx::Row row = scopeRows.fetchOne())
                        scopes.emplace_back(row.get(0).get<std::int64_t>(), row.get(1).get<int>());
                    return scopes;
                },
                [this](std::vector<std::pair<std::int64_t, int>> scopes)
                { m_factionService.loadScopes(std::move(scopes)); },
                [](const std::string &error)
                { LogManager::log(Error, "FactionSystem: failed to load rank scopes: " + error); });
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "FactionSystem: failed to load faction ranks: " + error);
        });

    DatabaseManager::selectQuery<std::vector<std::pair<int, std::int64_t>>>(
        [](mysqlx::Schema schema)
        {
            mysqlx::RowResult budgetRows = schema.getTable("faction_budget").select("faction_id", "budget").execute();
            std::vector<std::pair<int, std::int64_t>> budgets;
            while (mysqlx::Row row = budgetRows.fetchOne())
                budgets.emplace_back(row.get(0).get<int>(), row.get(1).get<std::int64_t>());
            return budgets;
        },
        [this](std::vector<std::pair<int, std::int64_t>> budgets)
        { m_factionService.loadBudgets(std::move(budgets)); },
        [](const std::string &error)
        {
            LogManager::log(Error, "FactionSystem: failed to load faction budgets: " + error);
        });
}

void FactionSystem::loadMembership(IPlayer &player, const PlayerSessionService::Session &session)
{
    DatabaseManager::selectQuery<std::tuple<int, std::int64_t, bool, std::int64_t, int>>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult result = schema.getTable("faction_member")
                                           .select("faction_id", "rank_id", "is_leader", "salary", "skin")
                                           .where("account_id = :account")
                                           .limit(1)
                                           .bind("account", accountId)
                                           .execute();

            int factionId = FactionService::NO_FACTION;
            std::int64_t rankId = 0;
            bool leader = false;
            std::int64_t salary = 0;
            int skin = 0;
            if (mysqlx::Row row = result.fetchOne())
            {
                factionId = row.get(0).get<int>();
                rankId = row.get(1).get<std::int64_t>();
                leader = row.get(2).get<int>() != 0;
                salary = row.get(3).get<std::int64_t>();
                skin = row.get(4).get<int>();
            }
            return std::make_tuple(factionId, rankId, leader, salary, skin);
        },
        [this, playerId = player.getID(), serial = session.serial,
         accountId = session.accountId](std::tuple<int, std::int64_t, bool, std::int64_t, int> membership)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/другая сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;

            const auto [factionId, rankId, leader, salary, skin] = membership;
            m_factionService.handleSessionStart(*player, accountId, factionId, rankId, leader, salary, skin);
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "FactionSystem: failed to load membership: " + error);
        });
}

// ------------------------------------------------------------------ команды лидера по людям

// Валидация цели найма (общая для /invite и кнопки «Нанять сотрудника»);
// успех — ведём в единую цепочку выбора ранга. false — отказ с сообщением.
bool FactionSystem::validateHireTarget(IPlayer &leader, int targetId)
{
    if (!permittedFaction(leader, FactionService::PERM_INVITE))
        return false;
    IPlayer *target = m_core.getPlayers().get(targetId);
    if (!target)
    {
        leader.sendClientMessage(ERROR_COLOUR, u("Игрок не найден"));
        return false;
    }
    if (m_factionService.getMemberFaction(targetId) != FactionService::NO_FACTION)
    {
        leader.sendClientMessage(ERROR_COLOUR, u("Игрок уже состоит во фракции"));
        return false;
    }
    const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
    if (!targetSession)
    {
        leader.sendClientMessage(ERROR_COLOUR, u("Игрок ещё не авторизован"));
        return false;
    }
    return true;
}

void FactionSystem::inviteMember(IPlayer &leader, int targetId)
{
    // /invite [id] — шорткат: сразу в ту же цепочку (id уже задан).
    if (!validateHireTarget(leader, targetId))
        return;
    showHireRankPick(leader, targetId, m_sessionService.get(targetId)->serial);
}

void FactionSystem::showHireInput(IPlayer &leader)
{
    // Единственное место с ручным вводом id — отсюда цепочка найма.
    if (!permittedFaction(leader, FactionService::PERM_INVITE))
        return;

    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Найм сотрудника");
    dialog.body = u("Введите id игрока для приёма во фракцию");
    dialog.leftButton = u("Далее");
    dialog.rightButton = u("Назад");

    m_dialogService.showNumberInput(
        leader, dialog,
        [this, leaderId = leader.getID()](DialogResponse response, std::int64_t targetId)
        {
            IPlayer *leader = m_core.getPlayers().get(leaderId);
            if (!leader)
                return;
            if (response != DialogResponse_Left)
            {
                showMembersMenu(*leader);
                return;
            }
            if (!validateHireTarget(*leader, static_cast<int>(targetId)))
            {
                showHireInput(*leader);
                return;
            }
            const int id = static_cast<int>(targetId);
            showHireRankPick(*leader, id, m_sessionService.get(id)->serial);
        });
}

void FactionSystem::showHireRankPick(IPlayer &leader, int targetId, std::uint32_t targetSerial)
{
    const FactionService::Faction *faction = permittedFaction(leader, FactionService::PERM_INVITE);
    if (!faction)
        return;
    IPlayer *target = m_core.getPlayers().get(targetId);
    if (!target)
        return;

    // Ранги пересобираем здесь же — список индексируется в колбэке.
    std::string body;
    for (const FactionService::Rank &rank : faction->ranks)
        body += fmt::format("{}\n", rank.name);
    body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(fmt::format("Найм {} — ранг", target->getName().to_string()));
    dialog.body = u(body);
    dialog.leftButton = u("Далее");
    dialog.rightButton = u("Отмена");

    m_dialogService.show(
        leader, dialog,
        [this, leaderId = leader.getID(), targetId, targetSerial](DialogResponse response, int listItem, StringView)
        {
            IPlayer *leader = m_core.getPlayers().get(leaderId);
            if (!leader || response != DialogResponse_Left)
                return;
            const FactionService::Faction *faction = permittedFaction(*leader, FactionService::PERM_INVITE);
            if (!faction)
                return;

            // Serial-guard цели: в её слот мог сесть другой игрок.
            IPlayer *target = m_core.getPlayers().get(targetId);
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            if (!target || !targetSession || targetSession->serial != targetSerial ||
                m_factionService.getMemberFaction(targetId) != FactionService::NO_FACTION)
            {
                leader->sendClientMessage(ERROR_COLOUR, u("Игрок недоступен для найма"));
                return;
            }
            if (listItem < 0 || static_cast<std::size_t>(listItem) >= faction->ranks.size())
                return;
            showHireSalaryInput(*leader, targetId, targetSerial, faction->ranks[listItem].id);
        });
}

void FactionSystem::showHireSalaryInput(IPlayer &leader, int targetId, std::uint32_t targetSerial, std::int64_t rankId)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Найм — зарплата");
    dialog.body = u(fmt::format("Укажите персональную зарплату нанимаемого (0..{}).\n"
                                "Зарплата платится из бюджета фракции.",
                                FactionService::MAX_SALARY));
    dialog.leftButton = u("Нанять");
    dialog.rightButton = u("Отмена");

    m_dialogService.showNumberInput(
        leader, dialog,
        [this, leaderId = leader.getID(), targetId, targetSerial, rankId](DialogResponse response, std::int64_t salary)
        {
            IPlayer *leader = m_core.getPlayers().get(leaderId);
            if (!leader || response != DialogResponse_Left)
                return;
            const FactionService::Faction *faction = permittedFaction(*leader, FactionService::PERM_INVITE);
            if (!faction)
                return;

            // Serial-guard цели: в её слот мог сесть другой игрок.
            IPlayer *target = m_core.getPlayers().get(targetId);
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            if (!target || !targetSession || targetSession->serial != targetSerial ||
                m_factionService.getMemberFaction(targetId) != FactionService::NO_FACTION)
            {
                leader->sendClientMessage(ERROR_COLOUR, u("Игрок недоступен для найма"));
                return;
            }
            // Выбранный ранг могли удалить, пока диалог зарплаты был открыт.
            const FactionService::Rank *rank = m_factionService.getRank(faction->id, rankId);
            if (!rank)
            {
                leader->sendClientMessage(ERROR_COLOUR, u("Выбранный ранг уже не существует"));
                return;
            }

            if (salary < 0 || salary > FactionService::MAX_SALARY)
            {
                leader->sendClientMessage(ERROR_COLOUR, u("Некорректная зарплата"));
                showHireSalaryInput(*leader, targetId, targetSerial, rankId);
                return;
            }

            if (!m_factionService.setMember(*target, faction->id, rankId, false, salary))
                return;
            leader->sendClientMessage(INFO_COLOUR, u(fmt::format("{} принят во фракцию: ранг «{}», зарплата ${}",
                                                                 target->getName().to_string(), rank->name, salary)));
            target->sendClientMessage(INFO_COLOUR, u(fmt::format("Вы приняты во фракцию «{}»: ранг «{}», зарплата ${}",
                                                                 faction->name, rank->name, salary)));
        });
}

void FactionSystem::showSetSalaryDialog(IPlayer &leader, int targetId, std::function<void(IPlayer &)> onClose)
{
    const FactionService::Faction *faction = permittedFaction(leader, FactionService::PERM_INVITE);
    if (!faction)
        return;
    IPlayer *target = m_core.getPlayers().get(targetId);
    if (!target || m_factionService.getMemberFaction(targetId) != faction->id)
    {
        leader.sendClientMessage(ERROR_COLOUR, u("Игрок не найден или не в вашей фракции"));
        return;
    }
    const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
    if (!targetSession)
        return;

    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u(fmt::format("Зарплата {}", target->getName().to_string()));
    dialog.body = u(fmt::format("Текущая зарплата: ${}\nВведите новую (0..{})",
                                m_factionService.getMemberSalary(targetId), FactionService::MAX_SALARY));
    dialog.leftButton = u("Сохранить");
    dialog.rightButton = u("Отмена");

    m_dialogService.showNumberInput(
        leader, dialog,
        [this, leaderId = leader.getID(), targetId, targetSerial = targetSession->serial, onClose](
            DialogResponse response, std::int64_t salary)
        {
            IPlayer *leader = m_core.getPlayers().get(leaderId);
            if (!leader)
                return;
            if (response != DialogResponse_Left) // отмена — возврат туда, откуда пришли
            {
                if (onClose)
                    onClose(*leader);
                return;
            }
            const FactionService::Faction *faction = permittedFaction(*leader, FactionService::PERM_INVITE);
            if (!faction)
                return;

            IPlayer *target = m_core.getPlayers().get(targetId);
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            if (!target || !targetSession || targetSession->serial != targetSerial ||
                m_factionService.getMemberFaction(targetId) != faction->id)
            {
                leader->sendClientMessage(ERROR_COLOUR, u("Игрок уже не в вашей фракции"));
                if (onClose)
                    onClose(*leader);
                return;
            }

            if (!m_factionService.setMemberSalary(*target, salary))
            {
                leader->sendClientMessage(ERROR_COLOUR, u("Некорректная зарплата"));
                showSetSalaryDialog(*leader, targetId, onClose); // повтор ввода, не теряя возврат
                return;
            }
            leader->sendClientMessage(INFO_COLOUR,
                                      u(fmt::format("Зарплата {} теперь ${}", target->getName().to_string(), salary)));
            target->sendClientMessage(INFO_COLOUR, u(fmt::format("Ваша зарплата теперь ${}", salary)));
            if (onClose)
                onClose(*leader);
        });
}

void FactionSystem::uninviteMember(IPlayer &leader, int targetId)
{
    const FactionService::Faction *faction = permittedFaction(leader, FactionService::PERM_FIRE);
    if (!faction)
        return;
    IPlayer *target = m_core.getPlayers().get(targetId);
    if (!target || m_factionService.getMemberFaction(targetId) != faction->id)
    {
        leader.sendClientMessage(ERROR_COLOUR, u("Игрок не найден или не в вашей фракции"));
        return;
    }
    if (targetId == leader.getID())
    {
        leader.sendClientMessage(ERROR_COLOUR, u("Нельзя уволить самого себя"));
        return;
    }
    if (m_factionService.isLeader(targetId))
    {
        leader.sendClientMessage(ERROR_COLOUR, u("Лидера исключает только администрация"));
        return;
    }

    m_factionService.removeMember(*target);
    leader.sendClientMessage(INFO_COLOUR, u(fmt::format("{} исключён из фракции", target->getName().to_string())));
    target->sendClientMessage(INFO_COLOUR, u(fmt::format("Вы исключены из фракции «{}»", faction->name)));
}

void FactionSystem::showSetRankDialog(IPlayer &leader, int targetId, std::function<void(IPlayer &)> onClose)
{
    const FactionService::Faction *faction = leaderFaction(leader);
    if (!faction)
        return;
    IPlayer *target = m_core.getPlayers().get(targetId);
    if (!target || m_factionService.getMemberFaction(targetId) != faction->id)
    {
        leader.sendClientMessage(ERROR_COLOUR, u("Игрок не найден или не в вашей фракции"));
        return;
    }
    const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
    if (!targetSession)
        return;

    std::string body;
    for (const FactionService::Rank &rank : faction->ranks)
        body += fmt::format("{}\n", rank.name);
    body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(fmt::format("Ранг для {}", target->getName().to_string()));
    dialog.body = u(body);
    dialog.leftButton = u("Назначить");
    dialog.rightButton = u("Отмена");

    m_dialogService.show(
        leader, dialog,
        [this, leaderId = leader.getID(), targetId, targetSerial = targetSession->serial, onClose](
            DialogResponse response, int listItem, StringView)
        {
            IPlayer *leader = m_core.getPlayers().get(leaderId);
            if (!leader)
                return;
            if (response != DialogResponse_Left) // отмена — возврат туда, откуда пришли
            {
                if (onClose)
                    onClose(*leader);
                return;
            }
            const FactionService::Faction *faction = leaderFaction(*leader);
            if (!faction)
                return;

            // Serial-guard цели: в её слот мог сесть другой игрок, пока диалог открыт.
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            IPlayer *target = m_core.getPlayers().get(targetId);
            if (!target || !targetSession || targetSession->serial != targetSerial ||
                m_factionService.getMemberFaction(targetId) != faction->id)
            {
                leader->sendClientMessage(ERROR_COLOUR, u("Игрок уже не в вашей фракции"));
                if (onClose)
                    onClose(*leader);
                return;
            }
            if (listItem < 0 || static_cast<std::size_t>(listItem) >= faction->ranks.size())
            {
                if (onClose)
                    onClose(*leader);
                return;
            }

            const FactionService::Rank &rank = faction->ranks[listItem];
            if (!m_factionService.setMemberRank(*target, rank.id))
                return;
            leader->sendClientMessage(
                INFO_COLOUR, u(fmt::format("{} назначен на ранг «{}»", target->getName().to_string(), rank.name)));
            target->sendClientMessage(INFO_COLOUR, u(fmt::format("Ваш новый ранг — «{}»", rank.name)));
            if (onClose)
                onClose(*leader);
        });
}

// ------------------------------------------------------------------ меню «Сотрудники»

void FactionSystem::showMembersMenu(IPlayer &player)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;

    // Собираем членов ОНЛАЙН и параллельно их id — список индексируется в
    // колбэке, и порядок entries() между показом и ответом не гарантирован,
    // поэтому id сохраняем здесь же (и заново сверяем в карточке).
    std::vector<int> memberIds;
    std::string body = "Имя\tРанг\tЗарплата\n";
    for (IPlayer *member : m_core.getPlayers().entries())
    {
        const int memberId = member->getID();
        if (m_factionService.getMemberFaction(memberId) != faction->id)
            continue;
        const FactionService::Rank *rank = m_factionService.getMemberRank(memberId);
        body += fmt::format("{}\t{}\t${}\n", member->getName().to_string(), rank ? rank->name : "?",
                            m_factionService.getMemberSalary(memberId));
        memberIds.push_back(memberId);
    }
    body += "» Нанять сотрудника";

    Dialog dialog;
    dialog.style = DialogStyle_TABLIST_HEADERS;
    dialog.title = u(fmt::format("{} — сотрудники", faction->name));
    dialog.body = u(body);
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), memberIds = std::move(memberIds)](DialogResponse response, int listItem,
                                                                            StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showLeaderMenu(*player);
                return;
            }
            if (!m_factionService.isLeader(playerId))
                return;
            // Последняя строка — найм; всё, что выше, — выбранный сотрудник.
            if (listItem < 0 || static_cast<std::size_t>(listItem) >= memberIds.size())
            {
                showHireInput(*player);
                return;
            }
            const int targetId = memberIds[listItem];
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            if (!targetSession)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Сотрудник уже не в сети"));
                showMembersMenu(*player);
                return;
            }
            showMemberCard(*player, targetId, targetSession->serial);
        });
}

void FactionSystem::showMemberCard(IPlayer &player, int targetId, std::uint32_t targetSerial)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;
    // Serial-guard: в слот выбранного мог сесть другой игрок, пока шли диалоги.
    IPlayer *target = m_core.getPlayers().get(targetId);
    const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
    if (!target || !targetSession || targetSession->serial != targetSerial ||
        m_factionService.getMemberFaction(targetId) != faction->id)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сотрудник уже не в вашей фракции"));
        showMembersMenu(player);
        return;
    }

    const FactionService::Rank *rank = m_factionService.getMemberRank(targetId);
    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(fmt::format("{} — ранг «{}», ${}", target->getName().to_string(), rank ? rank->name : "?",
                                 m_factionService.getMemberSalary(targetId)));
    dialog.body = u("Изменить ранг\nИзменить зарплату\nУволить");
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), targetId, targetSerial](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showMembersMenu(*player);
                return;
            }
            const FactionService::Faction *faction = leaderFaction(*player);
            if (!faction)
                return;
            // Перепроверка цели заново: операции ниже сверяют serial сами
            // (showSetRankDialog/showSetSalaryDialog/uninviteMember), но карточка
            // могла устареть — отсекаем сразу.
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            if (!m_core.getPlayers().get(targetId) || !targetSession || targetSession->serial != targetSerial ||
                m_factionService.getMemberFaction(targetId) != faction->id)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Сотрудник уже не в вашей фракции"));
                showMembersMenu(*player);
                return;
            }
            // Возврат к карточке после ранга/зарплаты — навигация без тупика
            // (serial цели операцией не меняется, карточка перепроверит сама).
            auto backToCard = [this, targetId, targetSerial](IPlayer &p) { showMemberCard(p, targetId, targetSerial); };
            switch (listItem)
            {
            case 0:
                showSetRankDialog(*player, targetId, backToCard);
                break;
            case 1:
                showSetSalaryDialog(*player, targetId, backToCard);
                break;
            case 2:
                // Уволенного в карточку не вернуть — назад к списку сотрудников.
                uninviteMember(*player, targetId);
                showMembersMenu(*player);
                break;
            default:
                break;
            }
        });
}

void FactionSystem::showMyRankMenu(IPlayer &player)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;

    // Ранги пересобираем здесь — список индексируется в колбэке.
    std::string body;
    for (const FactionService::Rank &rank : faction->ranks)
        body += fmt::format("{}\n", rank.name);
    body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Мой ранг");
    dialog.body = u(body);
    dialog.leftButton = u("Назначить");
    dialog.rightButton = u("Назад");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response != DialogResponse_Left)
                             {
                                 showLeaderMenu(*player);
                                 return;
                             }
                             // Лидерство могли снять, ранги — измениться.
                             const FactionService::Faction *faction = leaderFaction(*player);
                             if (!faction || listItem < 0 ||
                                 static_cast<std::size_t>(listItem) >= faction->ranks.size())
                                 return;
                             const FactionService::Rank &rank = faction->ranks[listItem];
                             if (!m_factionService.setMemberRank(*player, rank.id))
                                 return;
                             player->sendClientMessage(INFO_COLOUR,
                                                       u(fmt::format("Ваш ранг теперь — «{}»", rank.name)));
                         });
}

// ------------------------------------------------------------------ меню куратора /gov

void FactionSystem::showGovMenu(IPlayer &player)
{
    const auto managed = m_factionService.managedBy(player.getID());
    if (managed.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вам не подчиняются фракции"));
        return;
    }

    std::string body;
    for (const auto *faction : managed)
    {
        const int leaderId = m_factionService.onlineLeaderId(faction->id);
        IPlayer *leader = leaderId >= 0 ? m_core.getPlayers().get(leaderId) : nullptr;
        body += fmt::format("{} — лидер: {}\n", faction->name, leader ? leader->getName().to_string() : "не в сети");
    }
    body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Подопечные фракции");
    dialog.body = u(body);
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player || response != DialogResponse_Left)
                                 return;
                             const auto managed = m_factionService.managedBy(playerId);
                             if (listItem < 0 || static_cast<std::size_t>(listItem) >= managed.size())
                                 return;
                             showGovFactionMenu(*player, managed[listItem]->id);
                         });
}

void FactionSystem::showGovFactionMenu(IPlayer &player, int factionId)
{
    const FactionService::Faction *faction = m_factionService.getFaction(factionId);
    if (!faction || !m_factionService.canManage(player.getID(), factionId))
        return;

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(faction->name);
    dialog.body = u("Назначить лидера\nСнять лидера\nУволить сотрудника");
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Назад");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID(), factionId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response != DialogResponse_Left)
                             {
                                 showGovMenu(*player);
                                 return;
                             }
                             if (!m_factionService.canManage(playerId, factionId))
                                 return;
                             if (listItem == 0)
                                 showGovAppointInput(*player, factionId);
                             else if (listItem == 1)
                                 govDismissLeader(*player, factionId);
                             else if (listItem == 2)
                                 showGovMembersMenu(*player, factionId);
                         });
}

void FactionSystem::showGovAppointInput(IPlayer &player, int factionId)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Назначение лидера");
    dialog.body = u("Введите id игрока.\nИгрок должен быть в сети и не состоять в другой фракции.");
    dialog.leftButton = u("Далее");
    dialog.rightButton = u("Назад");

    m_dialogService.showNumberInput(
        player, dialog,
        [this, playerId = player.getID(), factionId](DialogResponse response, std::int64_t targetId)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showGovFactionMenu(*player, factionId);
                return;
            }
            if (!m_factionService.canManage(playerId, factionId))
                return;

            IPlayer *target = m_core.getPlayers().get(static_cast<int>(targetId));
            const PlayerSessionService::Session *targetSession =
                target ? m_sessionService.get(target->getID()) : nullptr;
            if (!target || !targetSession)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Игрок не найден или не авторизован"));
                showGovAppointInput(*player, factionId);
                return;
            }
            const int targetFaction = m_factionService.getMemberFaction(target->getID());
            if (targetFaction != FactionService::NO_FACTION && targetFaction != factionId)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Игрок состоит в другой фракции"));
                return;
            }
            showGovAppointSalaryInput(*player, factionId, target->getID(), targetSession->serial);
        });
}

void FactionSystem::showGovAppointSalaryInput(IPlayer &player, int factionId, int targetId, std::uint32_t targetSerial)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Зарплата лидера");
    dialog.body =
        u(fmt::format("Укажите зарплату лидера (0..{}).\nПлатится из бюджета фракции.", FactionService::MAX_SALARY));
    dialog.leftButton = u("Назначить");
    dialog.rightButton = u("Отмена");

    m_dialogService.showNumberInput(
        player, dialog,
        [this, playerId = player.getID(), factionId, targetId, targetSerial](DialogResponse response,
                                                                             std::int64_t salary)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;
            if (!m_factionService.canManage(playerId, factionId))
                return;

            IPlayer *target = m_core.getPlayers().get(targetId);
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            if (!target || !targetSession || targetSession->serial != targetSerial)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Игрок уже недоступен"));
                return;
            }

            if (salary < 0 || salary > FactionService::MAX_SALARY)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Некорректная зарплата"));
                showGovAppointSalaryInput(*player, factionId, targetId, targetSerial);
                return;
            }

            // Прежний лидер (если онлайн) узнаёт о снятии.
            const int oldLeaderId = m_factionService.onlineLeaderId(factionId);
            if (!m_factionService.appointLeader(*target, factionId, salary))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Не вышло назначить лидера"));
                return;
            }

            const FactionService::Faction *faction = m_factionService.getFaction(factionId);
            const std::string factionName = faction ? faction->name : "?";
            if (IPlayer *oldLeader =
                    oldLeaderId >= 0 && oldLeaderId != targetId ? m_core.getPlayers().get(oldLeaderId) : nullptr)
            {
                oldLeader->sendClientMessage(INFO_COLOUR,
                                             u(fmt::format("Вы сняты с поста лидера фракции «{}»", factionName)));
            }
            player->sendClientMessage(INFO_COLOUR, u(fmt::format("{} назначен лидером фракции «{}»",
                                                                 target->getName().to_string(), factionName)));
            target->sendClientMessage(
                INFO_COLOUR, u(fmt::format("Вы назначены лидером фракции «{}», зарплата ${}", factionName, salary)));
        });
}

void FactionSystem::govDismissLeader(IPlayer &player, int factionId)
{
    const int leaderId = m_factionService.onlineLeaderId(factionId);
    IPlayer *leader = leaderId >= 0 ? m_core.getPlayers().get(leaderId) : nullptr;
    if (!leader)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Лидер фракции не в сети"));
        return;
    }
    if (!m_factionService.dismissLeader(*leader))
        return;

    const FactionService::Faction *faction = m_factionService.getFaction(factionId);
    const std::string factionName = faction ? faction->name : "?";
    player.sendClientMessage(
        INFO_COLOUR, u(fmt::format("{} снят с поста лидера фракции «{}»", leader->getName().to_string(), factionName)));
    leader->sendClientMessage(INFO_COLOUR, u(fmt::format("Вы сняты с поста лидера фракции «{}»", factionName)));
}

void FactionSystem::showGovMembersMenu(IPlayer &player, int factionId)
{
    const FactionService::Faction *faction = m_factionService.getFaction(factionId);
    if (!faction || !m_factionService.canManage(player.getID(), factionId))
        return;

    // Собираем членов подопечной ОНЛАЙН и параллельно их id — список индексируется
    // в колбэке, а порядок entries() между показом и ответом не гарантирован,
    // поэтому id фиксируем здесь по значению (и заново сверяем в подтверждении).
    // Куратор сам в подопечной не состоит, но самого себя из списка исключаем на
    // всякий случай — уволить себя он не должен.
    const int curatorId = player.getID();
    std::vector<int> memberIds;
    std::string body = "Имя\tРанг\n";
    for (IPlayer *member : m_core.getPlayers().entries())
    {
        const int memberId = member->getID();
        if (memberId == curatorId)
            continue;
        if (m_factionService.getMemberFaction(memberId) != faction->id)
            continue;
        const FactionService::Rank *rank = m_factionService.getMemberRank(memberId);
        // Лидера помечаем — куратор уволит и его (это и есть смысл: снять-лидера
        // оставляет в штате, а здесь полное исключение).
        const char *marker = m_factionService.isLeader(memberId) ? " [лидер]" : "";
        body += fmt::format("{}{}\t{}\n", member->getName().to_string(), marker, rank ? rank->name : "?");
        memberIds.push_back(memberId);
    }

    if (memberIds.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("В подопечной фракции нет сотрудников в сети"));
        showGovFactionMenu(player, factionId);
        return;
    }
    body.pop_back(); // убрать хвостовой '\n' — иначе пустая строка-фантом в tablist

    Dialog dialog;
    dialog.style = DialogStyle_TABLIST_HEADERS;
    dialog.title = u(fmt::format("{} — увольнение", faction->name));
    dialog.body = u(body);
    dialog.leftButton = u("Уволить");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), factionId, memberIds = std::move(memberIds)](DialogResponse response,
                                                                                       int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showGovFactionMenu(*player, factionId);
                return;
            }
            // Право куратора могли срезать, пока диалог был открыт.
            if (!m_factionService.canManage(playerId, factionId))
                return;
            // Индекс маппим через снимок id из лямбды — НЕ итерируем entries()
            // заново (её порядок мог измениться). Bounds-check обязателен.
            if (listItem < 0 || static_cast<std::size_t>(listItem) >= memberIds.size())
                return;
            const int targetId = memberIds[listItem];
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            if (!targetSession)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Сотрудник уже не в сети"));
                showGovMembersMenu(*player, factionId);
                return;
            }
            showGovDismissConfirm(*player, factionId, targetId, targetSession->serial);
        });
}

void FactionSystem::showGovDismissConfirm(IPlayer &player, int factionId, int targetId, std::uint32_t targetSerial)
{
    const FactionService::Faction *faction = m_factionService.getFaction(factionId);
    if (!faction || !m_factionService.canManage(player.getID(), factionId))
        return;

    // Serial-guard: в слот выбранного мог сесть другой игрок, пока шёл список.
    // И цель должна всё ещё состоять ИМЕННО в этой подопечной фракции.
    IPlayer *target = m_core.getPlayers().get(targetId);
    const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
    if (!target || !targetSession || targetSession->serial != targetSerial || targetId == player.getID() ||
        m_factionService.getMemberFaction(targetId) != faction->id)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сотрудник уже недоступен"));
        showGovMembersMenu(player, factionId);
        return;
    }

    const FactionService::Rank *rank = m_factionService.getMemberRank(targetId);
    const char *marker = m_factionService.isLeader(targetId) ? " (лидер фракции)" : "";

    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u("Увольнение сотрудника");
    dialog.body = u(fmt::format("Уволить {}{} (ранг «{}») из фракции «{}»?\nЭто полное исключение из организации.",
                                target->getName().to_string(), marker, rank ? rank->name : "?", faction->name));
    dialog.leftButton = u("Уволить");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), factionId, targetId, targetSerial](DialogResponse response, int, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showGovMembersMenu(*player, factionId);
                return;
            }
            // Перепроверяем право куратора заново — могли срезать при открытом диалоге.
            if (!m_factionService.canManage(playerId, factionId))
                return;

            const FactionService::Faction *faction = m_factionService.getFaction(factionId);
            if (!faction)
                return;
            // Перепроверяем цель: serial её сессии (в слот мог сесть другой игрок),
            // что она не сам куратор, и что она всё ещё член ИМЕННО этой подопечной.
            IPlayer *target = m_core.getPlayers().get(targetId);
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            if (!target || !targetSession || targetSession->serial != targetSerial || targetId == playerId ||
                m_factionService.getMemberFaction(targetId) != faction->id)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Сотрудник уже недоступен"));
                showGovMembersMenu(*player, factionId);
                return;
            }

            // Полное исключение: removeMember снимает и членство, и флаг лидера,
            // пишет в БД. Куратор выше внутрифракционного запрета «лидера увольняет
            // только администрация» — здесь он и есть эта администрация.
            const std::string targetName = target->getName().to_string();
            if (!m_factionService.removeMember(*target))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Не вышло уволить сотрудника"));
                showGovMembersMenu(*player, factionId);
                return;
            }
            player->sendClientMessage(
                INFO_COLOUR, u(fmt::format("{} уволен из фракции «{}»", targetName, faction->name)));
            target->sendClientMessage(INFO_COLOUR, u(fmt::format("Вы уволены из фракции «{}»", faction->name)));
            // Возврат к обновлённому списку — навигация без тупика.
            showGovMembersMenu(*player, factionId);
        });
}

// ------------------------------------------------------------------ дев-меню /fdev

void FactionSystem::showDevMenu(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Фракции — дев-меню");
    dialog.body = u("Список фракций\nПринять игрока во фракцию\nНазначить лидера\nИсключить из фракции\n"
                    "Установить бюджет\nТелепорт на спавн организации");
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player || response != DialogResponse_Left)
                                 return;
                             switch (listItem)
                             {
                             case 0:
                                 listFactions(*player);
                                 break;
                             case 1:
                                 showDevTargetInput(*player, DevAction::Invite);
                                 break;
                             case 2:
                                 showDevTargetInput(*player, DevAction::MakeLeader);
                                 break;
                             case 3:
                                 showDevKickInput(*player);
                                 break;
                             case 4:
                                 showDevBudgetPick(*player);
                                 break;
                             case 5:
                                 showDevSpawnTeleportPick(*player);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void FactionSystem::listFactions(IPlayer &player)
{
    const auto &factions = m_factionService.getFactions();
    if (factions.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Фракции не зарегистрированы"));
        return;
    }
    for (const auto &faction : factions)
    {
        player.sendClientMessage(INFO_COLOUR, u(fmt::format("{}: {} | рангов: {} | бюджет: ${}", faction.id,
                                                            faction.name, faction.ranks.size(), faction.budget)));
    }
}

void FactionSystem::showDevTargetInput(IPlayer &player, DevAction action)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u(action == DevAction::Invite ? "Принять во фракцию" : "Назначить лидера");
    dialog.body = u("Введите id игрока");
    dialog.leftButton = u("Далее");
    dialog.rightButton = u("Назад");

    m_dialogService.showNumberInput(
        player, dialog,
        [this, playerId = player.getID(), action](DialogResponse response, std::int64_t targetId)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showDevMenu(*player);
                return;
            }
            IPlayer *target = m_core.getPlayers().get(static_cast<int>(targetId));
            const PlayerSessionService::Session *targetSession =
                target ? m_sessionService.get(target->getID()) : nullptr;
            if (!target || !targetSession)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Игрок не найден или не авторизован"));
                showDevTargetInput(*player, action);
                return;
            }
            showDevFactionPick(*player, action, target->getID(), targetSession->serial);
        });
}

void FactionSystem::showDevFactionPick(IPlayer &player, DevAction action, int targetId, std::uint32_t targetSerial)
{
    const auto &factions = m_factionService.getFactions();
    if (factions.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Фракции не зарегистрированы"));
        return;
    }

    std::string body;
    for (const auto &faction : factions)
        body += fmt::format("{} (id {})\n", faction.name, faction.id);
    body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Выбор фракции");
    dialog.body = u(body);
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), action, targetId, targetSerial](DialogResponse response, int listItem,
                                                                          StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showDevMenu(*player);
                return;
            }
            const auto &factions = m_factionService.getFactions();
            if (listItem < 0 || static_cast<std::size_t>(listItem) >= factions.size())
                return;
            const int factionId = factions[listItem].id;

            IPlayer *target = m_core.getPlayers().get(targetId);
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            if (!target || !targetSession || targetSession->serial != targetSerial)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Игрок уже недоступен"));
                return;
            }
            const FactionService::Rank *rank = m_factionService.defaultRank(factionId);
            if (!rank)
            {
                player->sendClientMessage(ERROR_COLOUR, u("У фракции нет рангов"));
                return;
            }
            if (!m_factionService.setMember(*target, factionId, rank->id, action == DevAction::MakeLeader, 0))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Не вышло"));
                return;
            }
            player->sendClientMessage(
                INFO_COLOUR, u(fmt::format("{} {} (зарплата $0 — выставь /setsalary)", target->getName().to_string(),
                                           action == DevAction::MakeLeader ? "назначен лидером" : "принят")));
        });
}

void FactionSystem::showDevKickInput(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Исключить из фракции");
    dialog.body = u("Введите id игрока");
    dialog.leftButton = u("Исключить");
    dialog.rightButton = u("Назад");

    m_dialogService.showNumberInput(
        player, dialog,
        [this, playerId = player.getID()](DialogResponse response, std::int64_t targetId)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showDevMenu(*player);
                return;
            }
            IPlayer *target = m_core.getPlayers().get(static_cast<int>(targetId));
            if (!target || !m_factionService.removeMember(*target))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Игрок не найден или не во фракции"));
                return;
            }
            player->sendClientMessage(INFO_COLOUR, u("Игрок исключён из фракции"));
        });
}

void FactionSystem::showDevBudgetPick(IPlayer &player)
{
    const auto &factions = m_factionService.getFactions();
    if (factions.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Фракции не зарегистрированы"));
        return;
    }

    std::string body;
    for (const auto &faction : factions)
        body += fmt::format("{} — ${}\n", faction.name, faction.budget);
    body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Бюджет — выбор фракции");
    dialog.body = u(body);
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Назад");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response != DialogResponse_Left)
                             {
                                 showDevMenu(*player);
                                 return;
                             }
                             const auto &factions = m_factionService.getFactions();
                             if (listItem < 0 || static_cast<std::size_t>(listItem) >= factions.size())
                                 return;
                             showDevBudgetInput(*player, factions[listItem].id);
                         });
}

void FactionSystem::showDevBudgetInput(IPlayer &player, int factionId)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Установить бюджет");
    dialog.body = u(fmt::format("Текущий бюджет: ${}\nВведите новый (0..{})", m_factionService.getBudget(factionId),
                                FactionService::MAX_BUDGET));
    dialog.leftButton = u("Сохранить");
    dialog.rightButton = u("Назад");

    // Числовой ввод: парс делает сервис (мусор/overflow → повтор показа сам),
    // здесь остаётся только проверка диапазона через setBudget.
    m_dialogService.showNumberInput(
        player, dialog,
        [this, playerId = player.getID(), factionId](DialogResponse response, std::int64_t amount)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showDevBudgetPick(*player);
                return;
            }
            if (!m_factionService.setBudget(factionId, amount))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Некорректная сумма"));
                showDevBudgetInput(*player, factionId);
                return;
            }
            player->sendClientMessage(INFO_COLOUR, u(fmt::format("Бюджет фракции: ${}", amount)));
        });
}

void FactionSystem::showDevSpawnTeleportPick(IPlayer &player)
{
    // Список фильтруем: только фракции с заданной точкой спавна (у части —
    // напр. банков — спавна нет). listItem маппим по ЭТОМУ отфильтрованному
    // списку id, а не по индексу в getFactions(), поэтому копим id отдельно.
    std::string body;
    std::vector<int> spawnFactionIds;
    for (const FactionService::Faction &faction : m_factionService.getFactions())
    {
        if (!faction.spawn.defined)
            continue;
        spawnFactionIds.push_back(faction.id);
        body += fmt::format("{} (id {})\n", faction.name, faction.id);
    }
    if (spawnFactionIds.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Нет фракций с заданной точкой спавна"));
        showDevMenu(player);
        return;
    }
    body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Телепорт на спавн — выбор фракции");
    dialog.body = u(body);
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        // spawnFactionIds копируем в лямбду: список фракций может измениться
        // между показом и ответом — маппинг listItem -> id берём из снимка,
        // но сам спавн перепроверяем по актуальному getFaction ниже.
        [this, playerId = player.getID(), spawnFactionIds = std::move(spawnFactionIds)](DialogResponse response,
                                                                                       int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showDevMenu(*player);
                return;
            }
            if (listItem < 0 || static_cast<std::size_t>(listItem) >= spawnFactionIds.size())
                return;
            const int factionId = spawnFactionIds[listItem];

            // Перепроверяем фракцию и факт наличия спавна по актуальному
            // источнику правды — снимок мог устареть.
            const FactionService::Faction *faction = m_factionService.getFaction(factionId);
            if (!faction || !faction->spawn.defined)
            {
                player->sendClientMessage(ERROR_COLOUR, u("У фракции больше нет точки спавна"));
                return;
            }

            // Цель телепорта — СЕРВЕРНАЯ точка спавна фракции (не клиентский ввод).
            const FactionService::Spawn &spawn = faction->spawn;
            m_locationService.teleport(*player, spawn.position, static_cast<unsigned>(spawn.interior),
                                       spawn.virtualWorld);
            player->setRotation(GTAQuat(Vector3(0.0f, 0.0f, spawn.angle)));
            player->setCameraBehind();
            player->sendClientMessage(INFO_COLOUR,
                                      u(fmt::format("Телепорт на спавн «{}»", faction->name)));
        });
}

// ------------------------------------------------------------------ информация

void FactionSystem::showFactionInfo(IPlayer &player)
{
    const int factionId = m_factionService.getMemberFaction(player.getID());
    const FactionService::Faction *faction = m_factionService.getFaction(factionId);
    if (!faction)
    {
        player.sendClientMessage(INFO_COLOUR, u("Вы не состоите во фракции"));
        return;
    }
    const FactionService::Rank *rank = m_factionService.getMemberRank(player.getID());
    std::string rights;
    for (const FactionService::PermissionDef &def : m_factionService.permissionsOf(factionId))
    {
        if (m_factionService.hasPermission(player.getID(), def.mask))
            rights += def.name + ", ";
    }
    if (rights.size() >= 2)
        rights.resize(rights.size() - 2);
    else
        rights = "нет";
    player.sendClientMessage(
        INFO_COLOUR, u(fmt::format("Фракция: {} | ранг: {} | зарплата: ${}", faction->name, rank ? rank->name : "?",
                                   m_factionService.getMemberSalary(player.getID()))));
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Права: {}", rights)));
}

// ------------------------------------------------------------------ лидерский UI

const FactionService::Faction *FactionSystem::leaderFaction(IPlayer &player)
{
    if (!m_factionService.isLeader(player.getID()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы не лидер фракции"));
        return nullptr;
    }
    return m_factionService.getFaction(m_factionService.getMemberFaction(player.getID()));
}

const FactionService::Faction *FactionSystem::permittedFaction(IPlayer &player, FactionService::PermissionMask mask)
{
    if (!m_factionService.hasPermission(player.getID(), mask))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Недостаточно прав во фракции"));
        return nullptr;
    }
    return m_factionService.getFaction(m_factionService.getMemberFaction(player.getID()));
}

void FactionSystem::showLeaderMenu(IPlayer &player)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(fmt::format("{} — бюджет ${}", faction->name, faction->budget));
    // Порядок пунктов завязан на индексы в колбэке ниже — менять синхронно.
    dialog.body = u("Сотрудники\nМой ранг\nУправление рангами\nПриказ о выплате зарплат\nИнформация о фракции");
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player || response != DialogResponse_Left)
                                 return;
                             if (!m_factionService.isLeader(playerId))
                                 return;
                             switch (listItem)
                             {
                             case 0:
                                 showMembersMenu(*player);
                                 break;
                             case 1:
                                 showMyRankMenu(*player);
                                 break;
                             case 2:
                                 showRanksMenu(*player);
                                 break;
                             case 3:
                                 confirmPayOrder(*player);
                                 break;
                             case 4:
                                 showFactionInfo(*player);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void FactionSystem::showRanksMenu(IPlayer &player)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;

    const auto defs = m_factionService.permissionsOf(faction->id);
    std::string body = "Ранг\tДоступы\n";
    for (const FactionService::Rank &rank : faction->ranks)
    {
        std::string permissions;
        for (const FactionService::PermissionDef &def : defs)
        {
            if ((rank.permissions & def.mask) == def.mask)
                permissions += permissions.empty() ? def.name : ", " + def.name;
        }
        if (permissions.empty())
            permissions = "-";
        body += fmt::format("{}\t{}\n", rank.name, permissions);
    }
    body += "» Создать ранг";

    Dialog dialog;
    dialog.style = DialogStyle_TABLIST_HEADERS;
    dialog.title = u(fmt::format("{} — ранги", faction->name));
    dialog.body = u(body);
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), factionId = faction->id](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showLeaderMenu(*player);
                return;
            }
            // Перепроверка: лидерство могли снять, ранги — измениться.
            const FactionService::Faction *faction = leaderFaction(*player);
            if (!faction || faction->id != factionId)
                return;

            if (listItem >= 0 && static_cast<std::size_t>(listItem) < faction->ranks.size())
                showRankMenu(*player, faction->ranks[listItem].id);
            else
                showRankNameInput(*player, 0); // последняя строка — создание
        });
}

void FactionSystem::confirmPayOrder(IPlayer &player)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;
    if (!m_factionService.isPayOrderReady(faction->id, std::chrono::steady_clock::now()))
    {
        player.sendClientMessage(
            ERROR_COLOUR, u(fmt::format("Приказ о выплате можно отдавать не чаще раза в {} минут",
                                        FactionService::PAY_ORDER_COOLDOWN.count())));
        return;
    }

    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u("Приказ о выплате зарплат");
    dialog.body = u(fmt::format("Каждому члену фракции (включая оффлайн) будет зачислен чек на его\n"
                                "персональную зарплату на счёт в банке.\n\nБюджет фракции: ${}",
                                faction->budget));
    dialog.leftButton = u("Отдать приказ");
    dialog.rightButton = u("Отмена");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response != DialogResponse_Left)
                             {
                                 showLeaderMenu(*player);
                                 return;
                             }
                             executePayOrder(*player);
                         });
}

void FactionSystem::executePayOrder(IPlayer &player)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;
    const int factionId = faction->id;

    // Сумма зарплат — из БД (члены и оффлайн тоже). Списание и зачисление —
    // в колбэке; зарплаты, изменённые в эти миллисекунды, разойдутся на копейки.
    DatabaseManager::selectQuery<std::int64_t>(
        [factionId](mysqlx::Schema schema)
        {
            mysqlx::SqlResult result = schema.getSession()
                                           .sql("SELECT COALESCE(SUM(salary), 0) FROM faction_member WHERE faction_id = ?")
                                           .bind(factionId)
                                           .execute();
            mysqlx::Row row = result.fetchOne();
            return row ? row.get(0).get<std::int64_t>() : std::int64_t{0};
        },
        [this, leaderId = player.getID(), factionId](std::int64_t total)
        {
            IPlayer *leader = m_core.getPlayers().get(leaderId);
            if (!leader || !m_factionService.isLeader(leaderId) ||
                m_factionService.getMemberFaction(leaderId) != factionId)
                return;

            if (total <= 0)
            {
                leader->sendClientMessage(ERROR_COLOUR, u("Платить некому: ни у кого нет зарплаты"));
                return;
            }
            const TimePoint now = std::chrono::steady_clock::now();
            if (!m_factionService.isPayOrderReady(factionId, now))
                return; // двойной клик по диалогу
            if (!m_factionService.tryWithdraw(factionId, total))
            {
                leader->sendClientMessage(
                    ERROR_COLOUR, u(fmt::format("В бюджете не хватает: нужно ${}, есть ${}", total,
                                                m_factionService.getBudget(factionId))));
                return;
            }

            // Кулдаун — только после успешного списания: отказ из-за бюджета
            // не сжигает попытку.
            m_factionService.markPayOrderIssued(factionId, now);
            m_bankService.creditFactionSalaries(factionId);

            leader->sendClientMessage(INFO_COLOUR,
                                      u(fmt::format("Приказ исполнен: ${} списано с бюджета, чеки зачислены", total)));
            // Членам фракции онлайн — уведомление о чеке.
            for (IPlayer *member : m_core.getPlayers().entries())
            {
                const int memberId = member->getID();
                if (memberId == leaderId || m_factionService.getMemberFaction(memberId) != factionId)
                    continue;
                const std::int64_t salary = m_factionService.getMemberSalary(memberId);
                if (salary > 0)
                    member->sendClientMessage(
                        INFO_COLOUR, u(fmt::format("Вам начислена зарплата: чек на ${} на счёт в банке", salary)));
            }
        },
        [](const std::string &) { LogManager::log(Error, "FactionSystem: pay order failed"); });
}

void FactionSystem::showRankMenu(IPlayer &player, std::int64_t rankId)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;
    const FactionService::Rank *rank = m_factionService.getRank(faction->id, rankId);
    if (!rank)
    {
        showRanksMenu(player);
        return;
    }

    // Пункты: 0 — название, 1..N — тогглы доступов организации, дальше
    // «Подопечные организации» (только у фракций-кураторов) и удаление
    // (у стартового «Без ранга» пункта удаления нет).
    const auto defs = m_factionService.permissionsOf(faction->id);
    const bool curator = !m_factionService.subordinatesOf(faction->id).empty();

    std::string body = fmt::format("Название: {}\n", rank->name);
    for (const FactionService::PermissionDef &def : defs)
        body += fmt::format("{}: {}\n", def.name, (rank->permissions & def.mask) == def.mask ? "ВКЛ" : "выкл");
    if (curator)
        body += fmt::format("Подопечные организации: {}\n", rank->managed.size());
    if (rank->isDefault)
        body.pop_back(); // без «Удалить ранг»
    else
        body += "Удалить ранг";

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(fmt::format("Ранг «{}»", rank->name));
    dialog.body = u(body);
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), rankId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showRanksMenu(*player);
                return;
            }
            const FactionService::Faction *faction = leaderFaction(*player);
            if (!faction)
                return;
            const FactionService::Rank *rank = m_factionService.getRank(faction->id, rankId);
            if (!rank)
            {
                showRanksMenu(*player);
                return;
            }

            if (listItem == 0)
            {
                showRankNameInput(*player, rankId);
                return;
            }
            const auto defs = m_factionService.permissionsOf(faction->id);
            const bool curator = !m_factionService.subordinatesOf(faction->id).empty();
            std::size_t index = static_cast<std::size_t>(listItem) - 1;
            if (index < defs.size())
            {
                // Тоггл доступа: переключить и снова показать меню ранга.
                const FactionService::PermissionMask mask = defs[index].mask;
                const bool enabled = (rank->permissions & mask) == mask;
                m_factionService.editRankPermission(faction->id, rankId, mask, !enabled);
                showRankMenu(*player, rankId);
                return;
            }
            index -= defs.size();
            if (curator && index == 0)
            {
                showRankScopeMenu(*player, rankId);
                return;
            }
            if (curator)
                --index;
            if (index == 0 && !rank->isDefault)
                showRankDeleteConfirm(*player, rankId);
        });
}

void FactionSystem::showRankScopeMenu(IPlayer &player, std::int64_t rankId)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;
    const FactionService::Rank *rank = m_factionService.getRank(faction->id, rankId);
    const auto subordinates = m_factionService.subordinatesOf(faction->id);
    if (!rank || subordinates.empty())
    {
        showRanksMenu(player);
        return;
    }

    std::string body;
    for (const FactionService::Faction *subordinate : subordinates)
    {
        const bool managed =
            std::find(rank->managed.begin(), rank->managed.end(), subordinate->id) != rank->managed.end();
        body += fmt::format("{}: {}\n", subordinate->name, managed ? "В УПРАВЛЕНИИ" : "-");
    }
    body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(fmt::format("«{}» — подопечные организации", rank->name));
    dialog.body = u(body);
    dialog.leftButton = u("Переключить");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), rankId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showRankMenu(*player, rankId);
                return;
            }
            const FactionService::Faction *faction = leaderFaction(*player);
            if (!faction)
                return;
            const FactionService::Rank *rank = m_factionService.getRank(faction->id, rankId);
            const auto subordinates = m_factionService.subordinatesOf(faction->id);
            if (!rank || listItem < 0 || static_cast<std::size_t>(listItem) >= subordinates.size())
            {
                showRanksMenu(*player);
                return;
            }

            const int subordinateId = subordinates[listItem]->id;
            const bool managed =
                std::find(rank->managed.begin(), rank->managed.end(), subordinateId) != rank->managed.end();
            m_factionService.editRankScope(faction->id, rankId, subordinateId, !managed);
            showRankScopeMenu(*player, rankId);
        });
}

void FactionSystem::showRankNameInput(IPlayer &player, std::int64_t rankId)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u(rankId == 0 ? "Создание ранга" : "Название ранга");
    dialog.body = u("Введите название ранга (до 24 символов)");
    dialog.leftButton = u("Сохранить");
    dialog.rightButton = u("Назад");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID(), rankId](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response != DialogResponse_Left)
                             {
                                 rankId == 0 ? showRanksMenu(*player) : showRankMenu(*player, rankId);
                                 return;
                             }
                             const FactionService::Faction *faction = leaderFaction(*player);
                             if (!faction)
                                 return;

                             // Ввод клиента: cp1251 -> utf-8, чистка управляющих и \t.
                             const std::string name = FactionService::sanitizeRankName(
                                 Encoding::cp1251Toutf8(std::string(text.data(), text.size())));
                             if (name.empty())
                             {
                                 player->sendClientMessage(ERROR_COLOUR, u("Пустое название"));
                                 showRankNameInput(*player, rankId);
                                 return;
                             }

                             if (rankId == 0)
                             {
                                 const FactionService::Rank *created =
                                     m_factionService.createRank(faction->id, name);
                                 if (!created)
                                 {
                                     player->sendClientMessage(ERROR_COLOUR, u("Не вышло: достигнут лимит рангов"));
                                     showRanksMenu(*player);
                                     return;
                                 }
                                 // Сразу предлагаем пресет прав — частый случай быстрее ручного тоггла.
                                 showRankPresetMenu(*player, created->id);
                             }
                             else
                             {
                                 m_factionService.editRankName(faction->id, rankId, name);
                                 showRankMenu(*player, rankId);
                             }
                         });
}

void FactionSystem::showRankPresetMenu(IPlayer &player, std::int64_t rankId)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;
    const FactionService::Rank *rank = m_factionService.getRank(faction->id, rankId);
    if (!rank)
    {
        showRanksMenu(player);
        return;
    }

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(fmt::format("«{}» — права", rank->name));
    // Порядок строк завязан на индексы в колбэке — менять синхронно.
    dialog.body = u("Рядовой (без прав)\nОфицер (приём + скины)\nЗаместитель (все базовые права)\nНастроить вручную");
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Готово");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), rankId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            const FactionService::Faction *faction = leaderFaction(*player);
            if (!faction)
                return;
            const FactionService::Rank *rank = m_factionService.getRank(faction->id, rankId);
            if (!rank)
            {
                showRanksMenu(*player);
                return;
            }
            // Закрытие/«Готово» — ранг уже создан, просто к списку рангов.
            if (response != DialogResponse_Left)
            {
                showRanksMenu(*player);
                return;
            }

            // «Настроить вручную» — обычное меню ранга с тогглами.
            if (listItem == 3)
            {
                showRankMenu(*player, rankId);
                return;
            }

            // Пресет — набор базовых битов. Невыбранные базовые гасим, чтобы
            // пресет давал ровно то, что обещает (на свежесозданном ранге прав
            // нет, но пресет можно применить и повторно).
            FactionService::PermissionMask preset = 0;
            if (listItem == 1)
                preset = FactionService::PERM_INVITE | FactionService::PERM_SKIN; // офицер
            else if (listItem == 2)
                preset = FactionService::COMMON_PERMISSIONS; // заместитель — все базовые
            // listItem == 0 — рядовой: preset остаётся 0.

            for (const FactionService::PermissionMask mask :
                 {FactionService::PERM_INVITE, FactionService::PERM_FIRE, FactionService::PERM_BUDGET,
                  FactionService::PERM_SKIN})
            {
                m_factionService.editRankPermission(faction->id, rankId, mask, (preset & mask) != 0);
            }
            showRankMenu(*player, rankId);
        });
}

void FactionSystem::showRankDeleteConfirm(IPlayer &player, std::int64_t rankId)
{
    const FactionService::Faction *faction = leaderFaction(player);
    if (!faction)
        return;
    const FactionService::Rank *rank = m_factionService.getRank(faction->id, rankId);
    if (!rank)
    {
        showRanksMenu(player);
        return;
    }

    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u("Удаление ранга");
    dialog.body = u(fmt::format("Удалить ранг «{}»?\nЕго обладатели будут переведены на «Без ранга».", rank->name));
    dialog.leftButton = u("Удалить");
    dialog.rightButton = u("Назад");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID(), rankId](DialogResponse response, int, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response == DialogResponse_Left)
                             {
                                 const FactionService::Faction *faction = leaderFaction(*player);
                                 if (!faction)
                                     return;
                                 if (!m_factionService.deleteRank(faction->id, rankId))
                                     player->sendClientMessage(ERROR_COLOUR, u("Этот ранг удалить нельзя"));
                                 showRanksMenu(*player);
                                 return;
                             }
                             showRankMenu(*player, rankId);
                         });
}
