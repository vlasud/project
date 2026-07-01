#include "Services/FactionService/FactionService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerSkinService/PlayerSkinService.h"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>

// ------------------------------------------------------------------ справочник

void FactionService::registerFaction(int factionId, std::string name, int supervisorId)
{
    if (factionId <= NO_FACTION || name.empty() || factionId == supervisorId)
    {
        LogManager::log(Error, fmt::format("FactionService: invalid faction registration ({})", factionId));
        return;
    }
    if (getFaction(factionId))
    {
        LogManager::log(Error, fmt::format("FactionService: duplicate faction id {}", factionId));
        return;
    }

    Faction faction;
    faction.id = factionId;
    faction.name = std::move(name);
    faction.supervisorId = supervisorId;
    m_factions.push_back(std::move(faction));
}

void FactionService::registerBase(int factionId, const Base &base)
{
    Faction *faction = findFaction(factionId);
    if (!faction)
    {
        LogManager::log(Error, fmt::format("FactionService: base registration for unknown faction {}", factionId));
        return;
    }
    faction->base = base;
    faction->base.defined = true;
}

void FactionService::registerSpawn(int factionId, const Spawn &spawn)
{
    Faction *faction = findFaction(factionId);
    if (!faction)
    {
        LogManager::log(Error, fmt::format("FactionService: spawn registration for unknown faction {}", factionId));
        return;
    }
    faction->spawn = spawn;
    faction->spawn.defined = true;
}

void FactionService::registerColour(int factionId, Colour colour)
{
    if (Faction *faction = findFaction(factionId))
        faction->colour = colour;
    else
        LogManager::log(Error, fmt::format("FactionService: colour registration for unknown faction {}", factionId));
}

void FactionService::registerSkins(int factionId, std::vector<int> skins)
{
    Faction *faction = findFaction(factionId);
    if (!faction)
    {
        LogManager::log(Error, fmt::format("FactionService: skins registration for unknown faction {}", factionId));
        return;
    }
    for (const int skin : skins)
    {
        // Единый источник правды о валидности скина (1..311 кроме 74; 0/CJ
        // запрещён) — чтобы пул не разошёлся с применяемым скином.
        if (PlayerSkinService::isValidSkin(skin))
            faction->skins.push_back(skin);
        else
            LogManager::log(Warning, fmt::format("FactionService: invalid skin {} for faction {}", skin, factionId));
    }
}

bool FactionService::canUseSkin(int playerId, int skin) const
{
    if (!hasPermission(playerId, PERM_SKIN))
        return false;
    const Faction *faction = getFaction(getMemberFaction(playerId));
    if (!faction)
        return false;
    return std::find(faction->skins.begin(), faction->skins.end(), skin) != faction->skins.end();
}

int FactionService::resolveOrgSkin(int factionId, int savedSkin) const
{
    const Faction *faction = getFaction(factionId);
    if (!faction || faction->skins.empty())
        return -1; // у организации нет пула — надевать нечего
    // Сохранённый скин годится только если он реально в пуле сейчас (пул мог
    // измениться в коде между заходами). Иначе — первый из пула.
    if (std::find(faction->skins.begin(), faction->skins.end(), savedSkin) != faction->skins.end())
        return savedSkin;
    return faction->skins.front();
}

void FactionService::registerPermission(int factionId, PermissionMask mask, std::string name)
{
    Faction *faction = findFaction(factionId);
    if (!faction || name.empty() || mask == 0 || (mask & ((1ull << FIRST_CUSTOM_BIT) - 1)) != 0)
    {
        LogManager::log(Error, fmt::format("FactionService: invalid permission registration for faction {}", factionId));
        return;
    }
    faction->customPermissions.push_back({mask, std::move(name)});
}

const FactionService::Faction *FactionService::getFaction(int factionId) const
{
    for (const Faction &faction : m_factions)
    {
        if (faction.id == factionId)
            return &faction;
    }
    return nullptr;
}

const std::vector<FactionService::Faction> &FactionService::getFactions() const
{
    return m_factions;
}

const FactionService::Rank *FactionService::getRank(int factionId, std::int64_t rankId) const
{
    return const_cast<FactionService *>(this)->findRank(factionId, rankId);
}

const FactionService::Rank *FactionService::defaultRank(int factionId) const
{
    const Faction *faction = getFaction(factionId);
    if (!faction || faction->ranks.empty())
        return nullptr;
    for (const Rank &rank : faction->ranks)
    {
        if (rank.isDefault)
            return &rank;
    }
    return &faction->ranks.front(); // не должно случаться: loadRanks гарантирует
}

std::vector<FactionService::PermissionDef> FactionService::permissionsOf(int factionId) const
{
    std::vector<PermissionDef> result = {
        {PERM_INVITE, "Приглашение игроков"},
        {PERM_FIRE, "Увольнение игроков"},
        {PERM_BUDGET, "Доступ к бюджету"},
        {PERM_SKIN, "Смена скина"},
    };
    if (const Faction *faction = getFaction(factionId))
    {
        result.insert(result.end(), faction->customPermissions.begin(), faction->customPermissions.end());
    }
    return result;
}

FactionService::Faction *FactionService::findFaction(int factionId)
{
    for (Faction &faction : m_factions)
    {
        if (faction.id == factionId)
            return &faction;
    }
    return nullptr;
}

FactionService::Rank *FactionService::findRank(int factionId, std::int64_t rankId)
{
    Faction *faction = findFaction(factionId);
    if (!faction)
        return nullptr;
    for (Rank &rank : faction->ranks)
    {
        if (rank.id == rankId)
            return &rank;
    }
    return nullptr;
}

FactionService::Rank *FactionService::findRankById(std::int64_t rankId)
{
    for (Faction &faction : m_factions)
    {
        for (Rank &rank : faction.ranks)
        {
            if (rank.id == rankId)
                return &rank;
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------ членство

int FactionService::getMemberFaction(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return NO_FACTION;
    return m_members[playerId].factionId;
}

const FactionService::Rank *FactionService::getMemberRank(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return nullptr;
    const Member &member = m_members[playerId];
    if (member.factionId == NO_FACTION)
        return nullptr;
    return getRank(member.factionId, member.rankId);
}

std::int64_t FactionService::getMemberSalary(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    return m_members[playerId].factionId != NO_FACTION ? m_members[playerId].salary : 0;
}

int FactionService::getMemberSkin(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    return m_members[playerId].factionId != NO_FACTION ? m_members[playerId].skin : 0;
}

bool FactionService::isLeader(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    return m_members[playerId].factionId != NO_FACTION && m_members[playerId].leader;
}

bool FactionService::hasPermission(int playerId, PermissionMask mask) const
{
    if (isLeader(playerId))
        return true;
    const Rank *rank = getMemberRank(playerId);
    return rank && (rank->permissions & mask) == mask;
}

bool FactionService::setMember(IPlayer &player, int factionId, std::int64_t rankId, bool leader, std::int64_t salary)
{
    Member &member = m_members[player.getID()];
    if (member.accountId == PlayerSessionService::NO_ACCOUNT)
        return false; // без сессии членству некуда сохраняться
    if (!getRank(factionId, rankId))
        return false;
    if (salary < 0 || salary > MAX_SALARY)
        return false;

    const int oldFactionId = member.factionId;
    member.factionId = factionId;
    member.rankId = rankId;
    member.salary = salary;
    member.leader = leader;
    // Найм со сменой фракции — выбора ещё нет: ставим ПЕРВЫЙ скин из пула
    // (0, если пула нет). Назначение лидером уже-члена сохраняет его выбор.
    if (oldFactionId != factionId)
    {
        const int resolved = resolveOrgSkin(factionId, 0);
        member.skin = resolved >= 0 ? resolved : 0;
    }

    DatabaseManager::throwQuery(
        [accountId = member.accountId, factionId, rankId, leader, salary, skin = member.skin](mysqlx::Schema schema)
        {
            mysqlx::Table table = schema.getTable("faction_member");
            table.remove().where("account_id = :account").bind("account", accountId).execute();
            table.insert("account_id", "faction_id", "rank_id", "is_leader", "salary", "skin")
                .values(accountId, factionId, rankId, leader ? 1 : 0, salary, skin)
                .execute();
        });

    // Назначение лидером уже-члена фракцию не меняет — событие не о чем.
    if (oldFactionId != factionId)
        notifyChange(player, oldFactionId, factionId);
    return true;
}

bool FactionService::removeMember(IPlayer &player)
{
    Member &member = m_members[player.getID()];
    if (member.factionId == NO_FACTION)
        return false;

    const int oldFactionId = member.factionId;
    member.factionId = NO_FACTION;
    member.rankId = 0;
    member.salary = 0;
    member.leader = false;
    member.skin = 0;

    DatabaseManager::throwQuery(
        [accountId = member.accountId](mysqlx::Schema schema)
        {
            schema.getTable("faction_member")
                .remove()
                .where("account_id = :account")
                .bind("account", accountId)
                .execute();
        });

    notifyChange(player, oldFactionId, NO_FACTION);
    return true;
}

bool FactionService::setMemberRank(IPlayer &player, std::int64_t rankId)
{
    Member &member = m_members[player.getID()];
    if (member.factionId == NO_FACTION || !getRank(member.factionId, rankId))
        return false;

    member.rankId = rankId;
    DatabaseManager::throwQuery(
        [accountId = member.accountId, rankId](mysqlx::Schema schema)
        {
            schema.getTable("faction_member")
                .update()
                .set("rank_id", rankId)
                .where("account_id = :account")
                .bind("account", accountId)
                .execute();
        });
    return true;
}

bool FactionService::setMemberSalary(IPlayer &player, std::int64_t salary)
{
    Member &member = m_members[player.getID()];
    if (member.factionId == NO_FACTION || salary < 0 || salary > MAX_SALARY)
        return false;

    member.salary = salary;
    DatabaseManager::throwQuery(
        [accountId = member.accountId, salary](mysqlx::Schema schema)
        {
            schema.getTable("faction_member")
                .update()
                .set("salary", salary)
                .where("account_id = :account")
                .bind("account", accountId)
                .execute();
        });
    return true;
}

bool FactionService::setMemberSkin(IPlayer &player, int skin)
{
    Member &member = m_members[player.getID()];
    if (member.factionId == NO_FACTION)
        return false;
    // Валидация против серверных фактов: скин обязан быть в пуле организации.
    // (Право PERM_SKIN проверяет вызывающий через canUseSkin — здесь только
    // принадлежность пулу, чтобы не записать мусор с клиента.)
    const Faction *faction = getFaction(member.factionId);
    if (!faction || std::find(faction->skins.begin(), faction->skins.end(), skin) == faction->skins.end())
        return false;

    // Скин не изменился — ни записи в БД, ни лишней работы (антифлуд /skin:
    // повторный выбор того же скина — no-op).
    if (member.skin == skin)
        return true;

    member.skin = skin;
    DatabaseManager::throwQuery(
        [accountId = member.accountId, skin](mysqlx::Schema schema)
        {
            schema.getTable("faction_member")
                .update()
                .set("skin", skin)
                .where("account_id = :account")
                .bind("account", accountId)
                .execute();
        });
    return true;
}

// ------------------------------------------------------------------ надзор

bool FactionService::canManage(int playerId, int factionId) const
{
    const Faction *faction = getFaction(factionId);
    if (!faction || faction->supervisorId == NO_FACTION)
        return false;
    if (getMemberFaction(playerId) != faction->supervisorId)
        return false;
    if (isLeader(playerId))
        return true; // лидер куратора (президент) управляет всеми подопечными

    // Ранг управляет только организациями, переданными ему лидером куратора.
    const Rank *rank = getMemberRank(playerId);
    return rank && std::find(rank->managed.begin(), rank->managed.end(), factionId) != rank->managed.end();
}

std::vector<const FactionService::Faction *> FactionService::subordinatesOf(int factionId) const
{
    std::vector<const Faction *> result;
    for (const Faction &faction : m_factions)
    {
        if (faction.supervisorId == factionId)
            result.push_back(&faction);
    }
    return result;
}

bool FactionService::editRankScope(int factionId, std::int64_t rankId, int subordinateId, bool enabled)
{
    Rank *rank = findRank(factionId, rankId);
    const Faction *subordinate = getFaction(subordinateId);
    if (!rank || !subordinate || subordinate->supervisorId != factionId)
        return false; // чужую/некурируемую организацию рангу не передать

    const auto it = std::find(rank->managed.begin(), rank->managed.end(), subordinateId);
    if (enabled == (it != rank->managed.end()))
        return true; // уже в нужном состоянии
    if (enabled)
        rank->managed.push_back(subordinateId);
    else
        rank->managed.erase(it);

    DatabaseManager::throwQuery(
        [rankId, subordinateId, enabled](mysqlx::Schema schema)
        {
            mysqlx::Table table = schema.getTable("faction_rank_scope");
            if (enabled)
                table.insert("rank_id", "faction_id").values(rankId, subordinateId).execute();
            else
                table.remove()
                    .where("rank_id = :rank AND faction_id = :faction")
                    .bind("rank", rankId)
                    .bind("faction", subordinateId)
                    .execute();
        });
    return true;
}

std::vector<const FactionService::Faction *> FactionService::managedBy(int playerId) const
{
    std::vector<const Faction *> result;
    for (const Faction &faction : m_factions)
    {
        if (canManage(playerId, faction.id))
            result.push_back(&faction);
    }
    return result;
}

int FactionService::onlineLeaderId(int factionId) const
{
    for (std::size_t playerId = 0; playerId < m_members.size(); ++playerId)
    {
        if (m_members[playerId].factionId == factionId && m_members[playerId].leader)
            return static_cast<int>(playerId);
    }
    return -1;
}

bool FactionService::appointLeader(IPlayer &target, int factionId, std::int64_t salary)
{
    const Faction *faction = getFaction(factionId);
    Member &member = m_members[target.getID()];
    if (!faction || member.accountId == PlayerSessionService::NO_ACCOUNT)
        return false;
    if (member.factionId != NO_FACTION && member.factionId != factionId)
        return false; // член чужой фракции — сперва увольнение оттуда
    if (salary < 0 || salary > MAX_SALARY)
        return false;

    // Ранг: свой, если уже член; иначе стартовый.
    std::int64_t rankId = member.factionId == factionId ? member.rankId : 0;
    if (rankId == 0)
    {
        const Rank *start = defaultRank(factionId);
        if (!start)
            return false;
        rankId = start->id;
    }

    // Прежние лидеры (онлайн-кэш и вся БД) становятся обычными членами —
    // лидер у фракции один.
    for (Member &other : m_members)
    {
        if (other.factionId == factionId)
            other.leader = false;
    }
    DatabaseManager::throwQuery(
        [factionId](mysqlx::Schema schema)
        {
            schema.getTable("faction_member")
                .update()
                .set("is_leader", 0)
                .where("faction_id = :faction")
                .bind("faction", factionId)
                .execute();
        });

    return setMember(target, factionId, rankId, true, salary);
}

bool FactionService::appointLeaderByAccount(AccountId accountId, int factionId, IPlayer *onlinePlayer)
{
    Faction *faction = findFaction(factionId);
    const Rank *startRank = defaultRank(factionId);
    if (!faction || !startRank || accountId == PlayerSessionService::NO_ACCOUNT)
        return false;

    // Прежние лидеры — обычные члены (кэш онлайн + вся БД).
    for (Member &other : m_members)
    {
        if (other.factionId == factionId)
            other.leader = false;
    }

    std::int64_t rankId = startRank->id;
    std::int64_t salary = 0;
    // Уже член ЭТОЙ фракции — выбор скина сохраняется; иначе свежее членство ->
    // первый из пула (0, если пула нет).
    int skin = resolveOrgSkin(factionId, 0);
    if (skin < 0)
        skin = 0;
    if (onlinePlayer)
    {
        const Member &member = m_members[onlinePlayer->getID()];
        if (member.factionId == factionId)
        {
            rankId = member.rankId; // уже член — ранг, зарплата и скин сохраняются
            salary = member.salary;
            skin = member.skin;
        }
    }

    DatabaseManager::throwQuery(
        [accountId, factionId, rankId, salary, skin](mysqlx::Schema schema)
        {
            mysqlx::Table table = schema.getTable("faction_member");
            table.update().set("is_leader", 0).where("faction_id = :faction").bind("faction", factionId).execute();
            table.remove().where("account_id = :account").bind("account", accountId).execute();
            table.insert("account_id", "faction_id", "rank_id", "is_leader", "salary", "skin")
                .values(accountId, factionId, rankId, 1, salary, skin)
                .execute();
        });

    if (onlinePlayer)
    {
        Member &member = m_members[onlinePlayer->getID()];
        const int oldFactionId = member.factionId;
        member.factionId = factionId;
        member.rankId = rankId;
        member.salary = salary;
        member.leader = true;
        member.skin = skin;
        if (oldFactionId != factionId)
            notifyChange(*onlinePlayer, oldFactionId, factionId);
    }
    return true;
}

bool FactionService::dismissLeader(IPlayer &target)
{
    Member &member = m_members[target.getID()];
    if (member.factionId == NO_FACTION || !member.leader)
        return false;

    member.leader = false;
    DatabaseManager::throwQuery(
        [accountId = member.accountId](mysqlx::Schema schema)
        {
            schema.getTable("faction_member")
                .update()
                .set("is_leader", 0)
                .where("account_id = :account")
                .bind("account", accountId)
                .execute();
        });
    return true;
}

// ------------------------------------------------------------------ бюджет

std::int64_t FactionService::getBudget(int factionId) const
{
    const Faction *faction = getFaction(factionId);
    return faction ? faction->budget : 0;
}

bool FactionService::setBudget(int factionId, std::int64_t amount)
{
    Faction *faction = findFaction(factionId);
    if (!faction || amount < 0 || amount > MAX_BUDGET)
        return false;
    faction->budget = amount;
    writeBudget(factionId, amount);
    return true;
}

bool FactionService::deposit(int factionId, std::int64_t amount)
{
    Faction *faction = findFaction(factionId);
    if (!faction || amount <= 0)
        return false;
    faction->budget = std::min(MAX_BUDGET, faction->budget + std::min(amount, MAX_BUDGET));
    writeBudget(factionId, faction->budget);
    return true;
}

bool FactionService::tryWithdraw(int factionId, std::int64_t amount)
{
    Faction *faction = findFaction(factionId);
    if (!faction || amount <= 0 || faction->budget < amount)
        return false;
    faction->budget -= amount;
    writeBudget(factionId, faction->budget);
    return true;
}

bool FactionService::isPayOrderReady(int factionId, TimePoint now) const
{
    const Faction *faction = getFaction(factionId);
    if (!faction)
        return false;
    // TimePoint{} — приказов с запуска сервера не было (часы могли только
    // что стартовать, разница с эпохой не показательна).
    return faction->orderIssuedAt == TimePoint{} || now - faction->orderIssuedAt >= PAY_ORDER_COOLDOWN;
}

void FactionService::markPayOrderIssued(int factionId, TimePoint now)
{
    if (Faction *faction = findFaction(factionId))
        faction->orderIssuedAt = now;
}

void FactionService::clearPayOrderCooldown(int factionId)
{
    if (Faction *faction = findFaction(factionId))
        faction->orderIssuedAt = TimePoint{}; // «приказов не было» — isPayOrderReady снова true
}

void FactionService::writeBudget(int factionId, std::int64_t budget)
{
    // Атомарный upsert одним запросом (faction_id — PRIMARY KEY): нет окна
    // DELETE+INSERT, в котором сбой воркера между удалением и вставкой потерял бы
    // строку бюджета (бюджет -> 0 на следующем старте).
    DatabaseManager::throwQuery(
        [factionId, budget](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO faction_budget (faction_id, budget) VALUES (?, ?) "
                     "ON DUPLICATE KEY UPDATE budget = VALUES(budget)")
                .bind(factionId, budget)
                .execute();
        },
        [factionId](const std::string &error)
        {
            LogManager::log(Error,
                            fmt::format("FactionService: failed to persist budget of faction {}: {}", factionId, error));
        });
}

// ------------------------------------------------------------------ ранги

const FactionService::Rank *FactionService::createRank(int factionId, const std::string &name)
{
    Faction *faction = findFaction(factionId);
    if (!faction || faction->ranks.size() >= MAX_RANKS)
        return nullptr;
    if (name.empty() || name.size() > MAX_RANK_NAME_BYTES)
        return nullptr;

    Rank rank;
    rank.id = m_nextRankId++;
    rank.name = name;
    faction->ranks.push_back(rank);

    DatabaseManager::throwQuery(
        [rank, factionId](mysqlx::Schema schema)
        {
            schema.getTable("faction_rank")
                .insert("id", "faction_id", "name", "permissions", "is_default")
                .values(rank.id, factionId, rank.name, static_cast<std::int64_t>(rank.permissions), 0)
                .execute();
        });
    return &faction->ranks.back();
}

bool FactionService::editRankName(int factionId, std::int64_t rankId, const std::string &name)
{
    Rank *rank = findRank(factionId, rankId);
    if (!rank || name.empty() || name.size() > MAX_RANK_NAME_BYTES)
        return false;

    rank->name = name;
    DatabaseManager::throwQuery(
        [rankId, name](mysqlx::Schema schema)
        { schema.getTable("faction_rank").update().set("name", name).where("id = :id").bind("id", rankId).execute(); });
    return true;
}

bool FactionService::editRankPermission(int factionId, std::int64_t rankId, PermissionMask mask, bool enabled)
{
    Faction *faction = findFaction(factionId);
    Rank *rank = findRank(factionId, rankId);
    if (!faction || !rank || mask == 0)
        return false;

    // Только базовые биты и зарегистрированные этой организацией: чужой/левый
    // бит через UI не выставить.
    PermissionMask allowed = COMMON_PERMISSIONS;
    for (const PermissionDef &custom : faction->customPermissions)
        allowed |= custom.mask;
    if ((mask & ~allowed) != 0)
        return false;

    rank->permissions = enabled ? (rank->permissions | mask) : (rank->permissions & ~mask);
    DatabaseManager::throwQuery(
        [rankId, permissions = rank->permissions](mysqlx::Schema schema)
        {
            schema.getTable("faction_rank")
                .update()
                .set("permissions", static_cast<std::int64_t>(permissions))
                .where("id = :id")
                .bind("id", rankId)
                .execute();
        });
    return true;
}

bool FactionService::deleteRank(int factionId, std::int64_t rankId)
{
    Faction *faction = findFaction(factionId);
    const Rank *rank = findRank(factionId, rankId);
    if (!faction || !rank || rank->isDefault)
        return false; // «Без ранга» не удаляется — фракция без рангов невозможна

    // Запасной ранг ищем после исключения удаляемого.
    std::erase_if(faction->ranks, [rankId](const Rank &rank) { return rank.id == rankId; });
    const Rank *fallback = defaultRank(factionId);

    // Онлайн-члены удалённого ранга — на стартовый.
    for (Member &member : m_members)
    {
        if (member.factionId == factionId && member.rankId == rankId)
            member.rankId = fallback->id;
    }

    DatabaseManager::throwQuery(
        [factionId, rankId, fallbackId = fallback->id](mysqlx::Schema schema)
        {
            // Сначала перевод членов (в т.ч. оффлайн), затем удаление ранга
            // и его скоупа подопечных организаций.
            schema.getTable("faction_member")
                .update()
                .set("rank_id", fallbackId)
                .where("faction_id = :faction AND rank_id = :rank")
                .bind("faction", factionId)
                .bind("rank", rankId)
                .execute();
            schema.getTable("faction_rank_scope").remove().where("rank_id = :id").bind("id", rankId).execute();
            schema.getTable("faction_rank").remove().where("id = :id").bind("id", rankId).execute();
        });
    return true;
}

// ------------------------------------------------------------------ прочее

void FactionService::subscribeMemberChange(MemberObserver observer)
{
    m_observers.push_back(std::move(observer));
}

void FactionService::notifyChange(IPlayer &player, int oldFactionId, int newFactionId)
{
    for (const MemberObserver &observer : m_observers)
        observer(player, oldFactionId, newFactionId);
}

std::string FactionService::sanitizeRankName(std::string_view raw)
{
    return Encoding::sanitizeUserText(raw, MAX_RANK_NAME_BYTES);
}

// ------------------------------------------------------------------ вызовы FactionSystem

void FactionService::loadRanks(std::vector<std::pair<int, Rank>> ranks)
{
    std::size_t loaded = 0;
    for (auto &[factionId, rank] : ranks)
    {
        // id рангов растут и для отброшенных рангов — их строки остаются в БД,
        // переиспользование id привело бы к коллизии.
        m_nextRankId = std::max(m_nextRankId, rank.id + 1);

        Faction *faction = findFaction(factionId);
        if (!faction)
        {
            // Фракцию убрали из кода, ранги в БД остались — не наша строка.
            LogManager::log(Warning, fmt::format("FactionService: rank {} of unregistered faction {}, skipped",
                                                 rank.id, factionId));
            continue;
        }
        faction->ranks.push_back(std::move(rank));
        ++loaded;
    }
    LogManager::log(Message,
                    fmt::format("FactionService: {} factions registered, {} ranks loaded", m_factions.size(), loaded));

    // У каждой фракции обязан быть стартовый «Без ранга» (нельзя удалить):
    // новая фракция в коде получает его автоматически, без ручного INSERT.
    for (Faction &faction : m_factions)
    {
        const bool hasDefault =
            std::any_of(faction.ranks.begin(), faction.ranks.end(), [](const Rank &rank) { return rank.isDefault; });
        if (hasDefault)
            continue;

        Rank rank;
        rank.id = m_nextRankId++;
        rank.name = "Без ранга";
        rank.isDefault = true;
        faction.ranks.insert(faction.ranks.begin(), rank);

        DatabaseManager::throwQuery(
            [rank, factionId = faction.id](mysqlx::Schema schema)
            {
                schema.getTable("faction_rank")
                    .insert("id", "faction_id", "name", "permissions", "is_default")
                    .values(rank.id, factionId, rank.name, 0, 1)
                    .execute();
            });
        LogManager::log(Message, fmt::format("FactionService: created default rank for faction {}", faction.id));
    }
}

void FactionService::loadScopes(std::vector<std::pair<std::int64_t, int>> scopes)
{
    for (const auto &[rankId, subordinateId] : scopes)
    {
        Rank *rank = findRankById(rankId);
        const Faction *subordinate = getFaction(subordinateId);
        // Битые строки (ранг удалили вместе с фракцией из кода, подопечную
        // перевесили на другого куратора) — просто не грузим.
        if (rank && subordinate)
            rank->managed.push_back(subordinateId);
    }
}

void FactionService::loadBudgets(std::vector<std::pair<int, std::int64_t>> budgets)
{
    for (const auto &[factionId, budget] : budgets)
    {
        Faction *faction = findFaction(factionId);
        if (faction)
            faction->budget = std::clamp<std::int64_t>(budget, 0, MAX_BUDGET);
    }
}

void FactionService::handleSessionStart(IPlayer &player, AccountId accountId, int factionId, std::int64_t rankId,
                                        bool leader, std::int64_t salary, int skin)
{
    Member &member = m_members[player.getID()];
    member.accountId = accountId;

    // Битая ссылка (фракцию/ранг удалили, пока игрок был оффлайн) — не член.
    if (factionId != NO_FACTION && !getRank(factionId, rankId))
    {
        LogManager::log(Warning, fmt::format("FactionService: account {} has stale membership {}/{}, dropped",
                                             accountId, factionId, rankId));
        factionId = NO_FACTION;
        rankId = 0;
        leader = false;
        salary = 0;
        skin = 0;
    }

    member.factionId = factionId;
    member.rankId = rankId;
    member.salary = std::clamp<std::int64_t>(salary, 0, MAX_SALARY);
    member.leader = leader;
    // Сырое значение из БД; на применение всегда идёт через resolveOrgSkin
    // (невалидный/не из пула -> первый из пула), фактический скин применяет
    // подписчик в notifyChange.
    member.skin = factionId != NO_FACTION ? skin : 0;

    if (factionId != NO_FACTION)
        notifyChange(player, NO_FACTION, factionId);
}

void FactionService::resetPlayer(int playerId)
{
    m_members[playerId] = Member{};
}
