#include "Services/ElectionService/ElectionService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>

namespace
{
std::int64_t nowUnix()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

// ------------------------------------------------------------------ партии

const std::vector<ElectionService::Party> &ElectionService::getParties() const
{
    return m_parties;
}

const ElectionService::Party *ElectionService::getParty(std::int64_t partyId) const
{
    return const_cast<ElectionService *>(this)->findParty(partyId);
}

ElectionService::Party *ElectionService::findParty(std::int64_t partyId)
{
    for (Party &party : m_parties)
    {
        if (party.id == partyId)
            return &party;
    }
    return nullptr;
}

const ElectionService::Party *ElectionService::getPartyByLeader(AccountId accountId) const
{
    for (const Party &party : m_parties)
    {
        if (party.leader == accountId)
            return &party;
    }
    return nullptr;
}

bool ElectionService::isNameTaken(const std::string &name) const
{
    for (const Party &party : m_parties)
    {
        if (party.name == name)
            return true;
    }
    return false;
}

const ElectionService::Party *ElectionService::createParty(AccountId leader, const std::string &leaderName,
                                                           const std::string &name, const std::string &description)
{
    if (leader == PlayerSessionService::NO_ACCOUNT || m_parties.size() >= MAX_PARTIES)
        return nullptr;
    if (name.empty() || name.size() > MAX_NAME_BYTES || description.size() > MAX_DESC_BYTES)
        return nullptr;
    if (isNameTaken(name) || getPartyByLeader(leader))
        return nullptr;

    Party party;
    party.id = m_nextPartyId++;
    party.name = name;
    party.description = description;
    party.leaderName = leaderName;
    party.leader = leader;
    m_parties.push_back(party);

    DatabaseManager::throwQuery(
        [party](mysqlx::Schema schema)
        {
            schema.getTable("party")
                .insert("id", "name", "description", "leader_account_id", "leader_name")
                .values(party.id, party.name, party.description, party.leader, party.leaderName)
                .execute();
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "ElectionService::createParty failed: " + error);
        });
    return &m_parties.back();
}

// ------------------------------------------------------------------ выборы

bool ElectionService::isActive() const
{
    return m_active;
}

std::int64_t ElectionService::remainingSeconds() const
{
    if (!m_active)
        return 0;
    return std::max<std::int64_t>(0, m_endsAtUnix - nowUnix());
}

bool ElectionService::start(Minutes duration)
{
    if (m_active || m_parties.empty() || duration.count() <= 0)
        return false;

    m_active = true;
    m_endsAtUnix = nowUnix() + std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    m_voted.clear();
    for (Party &party : m_parties)
        party.votes = 0;

    writeState();
    DatabaseManager::throwQuery(
        [](mysqlx::Schema schema)
        {
            schema.getSession().sql("DELETE FROM election_vote").execute();
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "ElectionService::start failed: " + error);
        });
    return true;
}

bool ElectionService::hasVoted(AccountId accountId) const
{
    return m_voted.count(accountId) != 0;
}

ElectionService::VoteStatus ElectionService::vote(AccountId accountId, std::int64_t partyId)
{
    if (!m_active || remainingSeconds() == 0)
        return VoteStatus::NotActive;
    if (hasVoted(accountId))
        return VoteStatus::AlreadyVoted;
    Party *party = findParty(partyId);
    if (!party)
        return VoteStatus::NoParty;

    m_voted.insert(accountId);
    ++party->votes;
    DatabaseManager::throwQuery(
        [accountId, partyId](mysqlx::Schema schema)
        {
            schema.getTable("election_vote").insert("account_id", "party_id").values(accountId, partyId).execute();
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "ElectionService::vote failed: " + error);
        });
    return VoteStatus::Ok;
}

const ElectionService::Party *ElectionService::finish()
{
    if (!m_active)
        return nullptr;
    m_active = false;
    m_endsAtUnix = 0;
    writeState();

    const Party *winner = nullptr;
    for (const Party &party : m_parties)
    {
        if (party.votes == 0)
            continue;
        if (!winner || party.votes > winner->votes)
            winner = &party;
        else if (party.votes == winner->votes)
            LogManager::log(Warning, fmt::format("ElectionService: tie between parties {} and {}, earlier one wins",
                                                 winner->id, party.id));
    }
    return winner;
}

void ElectionService::writeState()
{
    DatabaseManager::throwQuery(
        [active = m_active, endsAt = m_endsAtUnix](mysqlx::Schema schema)
        {
            mysqlx::Table table = schema.getTable("election");
            table.remove().where("id = 1").execute();
            table.insert("id", "active", "ends_at").values(1, active ? 1 : 0, endsAt).execute();
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "ElectionService::writeState failed: " + error);
        });
}

// ------------------------------------------------------------------ утилиты

std::string ElectionService::sanitizeText(std::string_view raw, std::size_t maxBytes)
{
    return Encoding::sanitizeUserText(raw, maxBytes);
}

// ------------------------------------------------------------------ загрузка

void ElectionService::loadParties(std::vector<Party> parties)
{
    m_parties = std::move(parties);
    for (const Party &party : m_parties)
        m_nextPartyId = std::max(m_nextPartyId, party.id + 1);
    LogManager::log(Message, fmt::format("ElectionService: {} parties loaded", m_parties.size()));
}

void ElectionService::loadState(bool active, std::int64_t endsAtUnix)
{
    m_active = active;
    m_endsAtUnix = endsAtUnix;
}

void ElectionService::loadVotes(std::vector<std::pair<AccountId, std::int64_t>> votes)
{
    for (const auto &[accountId, partyId] : votes)
    {
        m_voted.insert(accountId);
        if (Party *party = findParty(partyId))
            ++party->votes;
    }
}
