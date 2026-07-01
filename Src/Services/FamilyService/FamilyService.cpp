#include "Services/FamilyService/FamilyService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>

FamilyService::FamilyService()
{
    m_playerFamily.fill(NO_FAMILY);
    m_playerAccount.fill(PlayerSessionService::NO_ACCOUNT);
}

// ------------------------------------------------------------------ запрос состояния

int FamilyService::getFamilyId(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return NO_FAMILY;
    return m_playerFamily[playerId];
}

const FamilyService::Family *FamilyService::getFamily(int familyId) const
{
    const auto it = m_families.find(familyId);
    return it != m_families.end() ? &it->second : nullptr;
}

bool FamilyService::isOwner(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    const Family *family = getFamily(m_playerFamily[playerId]);
    if (!family)
        return false;
    // Владелец — аккаунт игрока в сети, совпавший с ownerAccountId семьи слота.
    return m_playerAccount[playerId] != PlayerSessionService::NO_ACCOUNT &&
           m_playerAccount[playerId] == family->ownerAccountId;
}

int FamilyService::familyByAccount(AccountId accountId) const
{
    const auto it = m_accountFamily.find(accountId);
    return it != m_accountFamily.end() ? it->second : NO_FAMILY;
}

void FamilyService::subscribeLoaded(LoadedObserver observer)
{
    if (!observer)
        return;
    // Поздняя подписка после загрузки — колбэк сразу (one-shot не теряется).
    if (m_loaded)
    {
        observer();
        return;
    }
    m_loadedObservers.push_back(std::move(observer));
}

FamilyService::Family *FamilyService::findFamily(int familyId)
{
    const auto it = m_families.find(familyId);
    return it != m_families.end() ? &it->second : nullptr;
}

// ------------------------------------------------------------------ валидация имени

std::string FamilyService::sanitizeName(std::string_view rawUtf8)
{
    // Сначала обезвреживаем цветокоды клиента ('{','}','~') — их раскрашивает
    // КЛИЕНТ при рендере client message, а имя уходит тегом [{название}] в /f.
    // Затем общая чистка (управляющие символы, края, граница utf-8, NAME_MAX).
    const std::string filtered = Encoding::neutralizeColorCodes(rawUtf8);
    // Байтовый кап с запасом (до 4 байт на символ utf-8) — реальный лимит по
    // СИМВОЛАМ держит validateName; иначе кириллица (2 байта) резалась бы вдвое.
    return Encoding::sanitizeUserText(filtered, NAME_MAX * 4);
}

namespace
{
// Число utf-8 символов (codepoint'ов) в уже санитизированной строке: длину
// валидируем по символам, а не байтам (кириллица — 2 байта).
std::size_t utf8Length(std::string_view s)
{
    std::size_t count = 0;
    for (const char c : s)
    {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
            ++count; // ведущий байт символа
    }
    return count;
}

// Регистронезависимое сравнение ASCII (кириллица сравнивается побайтно — для
// неё точное совпадение и так срабатывает; цель — не плодить «Корлеоне»/«корлеоне»).
bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}
} // namespace

bool FamilyService::validateName(std::string_view utf8Name) const
{
    const std::size_t length = utf8Length(utf8Name);
    return length >= NAME_MIN && length <= NAME_MAX;
}

bool FamilyService::nameTaken(std::string_view utf8Name, int exceptId) const
{
    for (const auto &[id, family] : m_families)
    {
        if (id == exceptId)
            continue;
        if (equalsIgnoreCaseAscii(family.name, utf8Name))
            return true;
    }
    return false;
}

// ------------------------------------------------------------------ операции

FamilyService::Result FamilyService::createFamily(IPlayer &player, AccountId accountId, std::string_view rawName)
{
    if (!m_loaded) // m_nextId ещё не по max(id) из БД — новый id мог бы конфликтовать
        return Result::NotLoaded;
    if (accountId == PlayerSessionService::NO_ACCOUNT)
        return Result::NoSession;
    if (familyByAccount(accountId) != NO_FAMILY)
        return Result::AlreadyInFamily;

    const std::string name = sanitizeName(rawName);
    if (!validateName(name))
        return Result::InvalidName;
    if (nameTaken(name, NO_FAMILY))
        return Result::NameTaken;

    const int playerId = player.getID();
    const long long now = static_cast<long long>(std::time(nullptr));
    const int id = m_nextId++;

    Family family;
    family.id = id;
    family.name = name;
    family.ownerAccountId = accountId;
    family.createdAt = now;
    Mem owner;
    owner.accountId = accountId;
    owner.name = player.getName().to_string();
    owner.joinedAt = now;
    family.members.push_back(std::move(owner));

    m_families.emplace(id, std::move(family));
    m_accountFamily[accountId] = id;
    if (playerId >= 0 && playerId < MAX_PLAYERS)
        m_playerFamily[playerId] = id;

    DatabaseManager::throwQuery(
        [id, name, accountId, now, ownerName = player.getName().to_string()](mysqlx::Schema schema)
        {
            // family + первая строка family_member (владелец) — одной транзакцией,
            // чтобы семья без владельца не осталась при сбое посередине.
            mysqlx::Session &session = schema.getSession();
            session.startTransaction();
            try
            {
                schema.getTable("family")
                    .insert("id", "name", "owner_account_id", "created_at")
                    .values(id, name, accountId, now)
                    .execute();
                schema.getTable("family_member")
                    .insert("account_id", "family_id", "name", "joined_at")
                    .values(accountId, id, ownerName, now)
                    .execute();
                session.commit();
            }
            catch (...)
            {
                session.rollback();
                throw;
            }
        },
        [](const std::string &error)
        { LogManager::log(Error, "FamilyService: create family failed: " + error); });

    return Result::Ok;
}

FamilyService::Result FamilyService::joinFamily(IPlayer &player, AccountId accountId, int familyId)
{
    if (!m_loaded) // состав семей ещё не догружен — членство недостоверно
        return Result::NotLoaded;
    if (accountId == PlayerSessionService::NO_ACCOUNT)
        return Result::NoSession;
    if (familyByAccount(accountId) != NO_FAMILY)
        return Result::AlreadyInFamily;

    Family *family = findFamily(familyId);
    if (!family)
        return Result::NotFound;
    if (family->members.size() >= MAX_MEMBERS)
        return Result::FamilyFull;

    const int playerId = player.getID();
    const long long now = static_cast<long long>(std::time(nullptr));
    const std::string memberName = player.getName().to_string();

    Mem member;
    member.accountId = accountId;
    member.name = memberName;
    member.joinedAt = now;
    family->members.push_back(std::move(member)); // joinedAt == now >= всех — порядок сохраняется

    m_accountFamily[accountId] = familyId;
    if (playerId >= 0 && playerId < MAX_PLAYERS)
        m_playerFamily[playerId] = familyId;

    DatabaseManager::throwQuery(
        [familyId, accountId, memberName, now](mysqlx::Schema schema)
        {
            schema.getTable("family_member")
                .insert("account_id", "family_id", "name", "joined_at")
                .values(accountId, familyId, memberName, now)
                .execute();
        },
        [](const std::string &error)
        { LogManager::log(Error, "FamilyService: join family failed: " + error); });

    return Result::Ok;
}

bool FamilyService::leaveFamily(IPlayer &player, AccountId accountId)
{
    const int familyId = familyByAccount(accountId);
    Family *family = findFamily(familyId);
    if (!family)
        return false;

    const int playerId = player.getID();
    const bool wasOwner = family->ownerAccountId == accountId;

    // Последний член (владелец один в семье) — роспуск семьи целиком.
    if (family->members.size() <= 1)
    {
        destroyFamily(familyId);
        return true;
    }

    // Убираем уходящего из состава.
    std::erase_if(family->members, [accountId](const Mem &m) { return m.accountId == accountId; });
    m_accountFamily.erase(accountId);
    if (playerId >= 0 && playerId < MAX_PLAYERS)
        m_playerFamily[playerId] = NO_FAMILY;

    AccountId newOwner = family->ownerAccountId;
    if (wasOwner)
    {
        // Владение переходит старейшему по joinedAt среди оставшихся: members
        // упорядочены по joinedAt (create/join добавляют в хвост, load сортирует),
        // erase порядок сохраняет — старейший всегда front.
        newOwner = family->members.front().accountId;
        family->ownerAccountId = newOwner;
    }

    DatabaseManager::throwQuery(
        [familyId, accountId, wasOwner, newOwner](mysqlx::Schema schema)
        {
            schema.getTable("family_member")
                .remove()
                .where("account_id = :account")
                .bind("account", accountId)
                .execute();
            if (wasOwner)
            {
                schema.getTable("family")
                    .update()
                    .set("owner_account_id", newOwner)
                    .where("id = :id")
                    .bind("id", familyId)
                    .execute();
            }
        },
        [](const std::string &error)
        { LogManager::log(Error, "FamilyService: leave family failed: " + error); });

    return false;
}

bool FamilyService::disbandFamily(int ownerPlayerId)
{
    if (!isOwner(ownerPlayerId)) // владелец сверяется по аккаунту слота
        return false;
    destroyFamily(m_playerFamily[ownerPlayerId]);
    return true;
}

// ------------------------------------------------------------------ внутреннее

void FamilyService::destroyFamily(int familyId)
{
    Family *family = findFamily(familyId);
    if (!family)
        return;

    // Снять онлайн-слоты всех членов в сети и индекс по аккаунтам.
    for (const Mem &member : family->members)
        m_accountFamily.erase(member.accountId);
    clearOnlineMembers(familyId);

    m_families.erase(familyId);

    DatabaseManager::throwQuery(
        [familyId](mysqlx::Schema schema)
        {
            mysqlx::Session &session = schema.getSession();
            session.startTransaction();
            try
            {
                schema.getTable("family_member")
                    .remove()
                    .where("family_id = :id")
                    .bind("id", familyId)
                    .execute();
                schema.getTable("family").remove().where("id = :id").bind("id", familyId).execute();
                session.commit();
            }
            catch (...)
            {
                session.rollback();
                throw;
            }
        },
        [](const std::string &error)
        { LogManager::log(Error, "FamilyService: disband family failed: " + error); });
}

void FamilyService::clearOnlineMembers(int familyId)
{
    for (int &slot : m_playerFamily)
    {
        if (slot == familyId)
            slot = NO_FAMILY;
    }
}

void FamilyService::reindexAccounts()
{
    m_accountFamily.clear();
    for (const auto &[id, family] : m_families)
    {
        for (const Mem &member : family.members)
            m_accountFamily[member.accountId] = id;
    }
}

// ------------------------------------------------------------------ сессия

void FamilyService::onSessionStart(int playerId, AccountId accountId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_playerAccount[playerId] = accountId;
    m_playerFamily[playerId] = familyByAccount(accountId);
}

void FamilyService::onSessionEnd(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_playerAccount[playerId] = PlayerSessionService::NO_ACCOUNT;
    m_playerFamily[playerId] = NO_FAMILY;
}

// ------------------------------------------------------------------ загрузка

void FamilyService::loadFamily(int id, std::string name, AccountId ownerAccountId, long long createdAt)
{
    Family family;
    family.id = id;
    family.name = std::move(name);
    family.ownerAccountId = ownerAccountId;
    family.createdAt = createdAt;
    m_families.emplace(id, std::move(family));
}

void FamilyService::loadMember(int familyId, AccountId accountId, std::string name, long long joinedAt)
{
    Family *family = findFamily(familyId);
    if (!family)
    {
        // family_member без family (битая строка) — не наша запись.
        LogManager::log(Warning, fmt::format("FamilyService: member {} of unknown family {}, skipped",
                                             accountId, familyId));
        return;
    }
    Mem member;
    member.accountId = accountId;
    member.name = std::move(name);
    member.joinedAt = joinedAt;
    family->members.push_back(std::move(member));
}

void FamilyService::finalizeLoad()
{
    std::size_t maxId = 0;
    // Снимаем семьи без членов (битые строки), сортируем состав по joinedAt и
    // подтягиваем владельца, если его строки членства нет (на старейшего).
    for (auto it = m_families.begin(); it != m_families.end();)
    {
        Family &family = it->second;
        if (family.members.empty())
        {
            LogManager::log(Warning, fmt::format("FamilyService: family {} has no members, dropped", family.id));
            it = m_families.erase(it);
            continue;
        }
        std::sort(family.members.begin(), family.members.end(),
                  [](const Mem &a, const Mem &b) { return a.joinedAt < b.joinedAt; });
        // Владелец должен быть членом; если строка владельца пропала — на старейшего.
        const bool ownerIsMember = std::any_of(family.members.begin(), family.members.end(),
                                               [&](const Mem &m) { return m.accountId == family.ownerAccountId; });
        if (!ownerIsMember)
        {
            family.ownerAccountId = family.members.front().accountId;
            const int familyId = family.id;
            const AccountId newOwner = family.ownerAccountId;
            DatabaseManager::throwQuery(
                [familyId, newOwner](mysqlx::Schema schema)
                {
                    schema.getTable("family")
                        .update()
                        .set("owner_account_id", newOwner)
                        .where("id = :id")
                        .bind("id", familyId)
                        .execute();
                },
                [](const std::string &error)
                { LogManager::log(Error, "FamilyService: fix owner update failed: " + error); });
        }
        maxId = std::max<std::size_t>(maxId, static_cast<std::size_t>(family.id));
        ++it;
    }
    m_nextId = static_cast<int>(maxId) + 1;
    reindexAccounts();
    // Только теперь m_nextId безопасен (по max(id) из БД) — открываем create/join.
    m_loaded = true;
    LogManager::log(Message, fmt::format("FamilyService: {} families loaded, nextId {}", m_families.size(), m_nextId));
    // Оповещаем one-shot подписчиков (ParkedVehicleSystem грузит parked_vehicle строго
    // после семей). Ровно один раз — finalizeLoad вызывается однократно.
    for (const LoadedObserver &observer : m_loadedObservers)
        observer();
    m_loadedObservers.clear();
}
