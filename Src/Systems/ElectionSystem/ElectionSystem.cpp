#include "Systems/ElectionSystem/ElectionSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <optional>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};
const Colour ANNOUNCE_COLOUR{255, 215, 90};

// Точки пока произвольные (мэрии городов) — переставим, когда займёмся картой.
const Vector3 PARTY_DESK_POS{1480.94f, -1772.07f, 18.80f}; // мэрия ЛС, стойка регистрации
const Vector3 URN_POSITIONS[] = {
    {1479.00f, -1768.00f, 18.80f},  // мэрия ЛС
    {-2282.60f, 531.70f, 35.02f},   // мэрия СФ
    {2106.00f, 2190.00f, 11.06f},   // ЛВ
};
constexpr int PICKUP_MODEL = 1239; // иконка «i»

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

ElectionSystem::ElectionSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_electionService(serviceRegister.getService<ElectionService>()),
      m_factionService(serviceRegister.getService<FactionService>()),
      m_pickupService(serviceRegister.getService<PickupService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_timerService(serviceRegister.getService<TimerService>())
{
    // Дев-меню админа (до системы ролей открыто, как и прочие дев-тулзы).
    serviceRegister.getService<PlayerCommandService>().add(
        "edev", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showDevMenu(player); }, {},
        "дев-меню выборов (меню)", PlayerCommandService::HelpCategory::Hidden);
}

void ElectionSystem::initialize(IComponentList *components)
{
    load();
    createPickups();
}

// ------------------------------------------------------------------ загрузка

void ElectionSystem::load()
{
    DatabaseManager::selectQuery<std::vector<ElectionService::Party>>(
        [](mysqlx::Schema schema)
        {
            mysqlx::RowResult rows =
                schema.getTable("party").select("id", "name", "description", "leader_account_id", "leader_name").execute();
            std::vector<ElectionService::Party> parties;
            while (mysqlx::Row row = rows.fetchOne())
            {
                ElectionService::Party party;
                party.id = row.get(0).get<std::int64_t>();
                party.name = row.get(1).get<std::string>();
                party.description = row.get(2).get<std::string>();
                party.leader = row.get(3).get<std::int64_t>();
                party.leaderName = row.get(4).get<std::string>();
                parties.push_back(std::move(party));
            }
            return parties;
        },
        [this](std::vector<ElectionService::Party> parties)
        {
            m_electionService.loadParties(std::move(parties));

            // Состояние и голоса — строго после партий (голоса ссылаются на них).
            DatabaseManager::selectQuery<std::optional<std::pair<bool, std::int64_t>>>(
                [](mysqlx::Schema schema)
                {
                    mysqlx::RowResult stateRows =
                        schema.getTable("election").select("active", "ends_at").where("id = 1").limit(1).execute();
                    std::optional<std::pair<bool, std::int64_t>> state;
                    if (mysqlx::Row row = stateRows.fetchOne())
                        state = std::make_pair(row.get(0).get<int>() != 0, row.get(1).get<std::int64_t>());
                    return state;
                },
                [this](std::optional<std::pair<bool, std::int64_t>> state)
                {
                    if (state)
                        m_electionService.loadState(state->first, state->second);

                    DatabaseManager::selectQuery<std::vector<std::pair<ElectionService::AccountId, std::int64_t>>>(
                        [](mysqlx::Schema schema)
                        {
                            mysqlx::RowResult voteRows =
                                schema.getTable("election_vote").select("account_id", "party_id").execute();
                            std::vector<std::pair<ElectionService::AccountId, std::int64_t>> votes;
                            while (mysqlx::Row row = voteRows.fetchOne())
                                votes.emplace_back(row.get(0).get<std::int64_t>(), row.get(1).get<std::int64_t>());
                            return votes;
                        },
                        [this](std::vector<std::pair<ElectionService::AccountId, std::int64_t>> votes)
                        {
                            m_electionService.loadVotes(std::move(votes));
                            resumeTimer();
                        },
                        [](const std::string &error)
                        { LogManager::log(Error, "ElectionSystem: failed to load votes: " + error); });
                },
                [](const std::string &error)
                { LogManager::log(Error, "ElectionSystem: failed to load election state: " + error); });
        },
        [](const std::string &error) { LogManager::log(Error, "ElectionSystem: failed to load parties: " + error); });
}

void ElectionSystem::resumeTimer()
{
    if (!m_electionService.isActive())
        return;
    // Сервер мог перезапуститься посреди выборов: дозапускаем таймер на
    // остаток; срок, истёкший за время даунтайма, завершает выборы сразу.
    const std::int64_t remaining = m_electionService.remainingSeconds();
    if (remaining <= 0)
    {
        finishElection();
        return;
    }
    m_finishTimer = m_timerService.setTimeout(std::chrono::seconds(remaining), [this] { finishElection(); });
}

void ElectionSystem::createPickups()
{
    m_pickupService.add(PICKUP_MODEL, 1, PARTY_DESK_POS, [this](IPlayer &player) { showPartyMenu(player); });
    for (const Vector3 &position : URN_POSITIONS)
        m_pickupService.add(PICKUP_MODEL, 1, position, [this](IPlayer &player) { showVoteMenu(player); });
}

// ------------------------------------------------------------------ финал

void ElectionSystem::finishElection()
{
    m_timerService.cancel(m_finishTimer);
    const ElectionService::Party *winner = m_electionService.finish();
    if (!winner)
    {
        m_core.getPlayers().sendClientMessageToAll(ANNOUNCE_COLOUR,
                                                   u("Выборы завершены: голосов нет, президент не избран"));
        return;
    }

    // Лидер партии может быть оффлайн — резолвим онлайн-игрока через сессию.
    IPlayer *onlineLeader = nullptr;
    const int onlineId = m_sessionService.playerByAccount(winner->leader);
    if (onlineId >= 0)
        onlineLeader = m_core.getPlayers().get(onlineId);

    m_factionService.appointLeaderByAccount(winner->leader, PRESIDENT_FACTION_ID, onlineLeader);

    m_core.getPlayers().sendClientMessageToAll(
        ANNOUNCE_COLOUR, u(fmt::format("Выборы завершены! Победила партия «{}» ({} голосов). Президент — {}",
                                       winner->name, winner->votes, winner->leaderName)));
}

// ------------------------------------------------------------------ меню партий

void ElectionSystem::showPartyMenu(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Регистрация партий");
    dialog.body = u(fmt::format("Список партий\nСоздать партию (взнос ${})", ElectionService::PARTY_COST));
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player || response != DialogResponse_Left)
                                 return;
                             if (listItem == 0)
                                 showPartyList(*player, false);
                             else if (listItem == 1)
                                 showPartyNameInput(*player);
                         });
}

void ElectionSystem::showPartyList(IPlayer &player, bool forVote)
{
    const auto &parties = m_electionService.getParties();
    if (parties.empty())
    {
        player.sendClientMessage(INFO_COLOUR, u("Партий пока нет"));
        return;
    }

    std::string body;
    for (const auto &party : parties)
        body += fmt::format("{} — лидер {}\n", party.name, party.leaderName);
    body.pop_back();

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u(forVote ? "Голосование" : "Партии");
    dialog.body = u(body);
    dialog.leftButton = u(forVote ? "Голосовать" : "Подробнее");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID(), forVote](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player || response != DialogResponse_Left)
                                 return;
                             const auto &parties = m_electionService.getParties();
                             if (listItem < 0 || static_cast<std::size_t>(listItem) >= parties.size())
                                 return;
                             const std::int64_t partyId = parties[listItem].id;
                             if (forVote)
                                 confirmVote(*player, partyId);
                             else
                                 showPartyDetails(*player, partyId);
                         });
}

void ElectionSystem::showPartyDetails(IPlayer &player, std::int64_t partyId)
{
    const ElectionService::Party *party = m_electionService.getParty(partyId);
    if (!party)
        return;

    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u(fmt::format("Партия «{}»", party->name));
    dialog.body = u(fmt::format("Лидер: {}\n\n{}", party->leaderName,
                                party->description.empty() ? "Без описания" : party->description));
    dialog.leftButton = u("Назад");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse, int, StringView)
                         {
                             if (IPlayer *player = m_core.getPlayers().get(playerId))
                                 showPartyList(*player, false);
                         });
}

void ElectionSystem::showPartyNameInput(IPlayer &player)
{
    if (!m_sessionService.isActive(player.getID()))
        return;
    if (m_electionService.getPartyByLeader(m_sessionService.getAccountId(player.getID())))
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас уже есть партия"));
        return;
    }

    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Партия — название");
    dialog.body = u("Введите название партии (до 24 символов)");
    dialog.leftButton = u("Далее");
    dialog.rightButton = u("Отмена");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;

            const std::string name = ElectionService::sanitizeText(
                Encoding::cp1251Toutf8(std::string(text.data(), text.size())), ElectionService::MAX_NAME_BYTES);
            if (name.empty())
            {
                player->sendClientMessage(ERROR_COLOUR, u("Пустое название"));
                showPartyNameInput(*player);
                return;
            }
            if (m_electionService.isNameTaken(name))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Партия с таким названием уже есть"));
                showPartyNameInput(*player);
                return;
            }
            showPartyDescriptionInput(*player, name);
        });
}

void ElectionSystem::showPartyDescriptionInput(IPlayer &player, const std::string &name)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Партия — описание");
    dialog.body = u("Опишите программу партии (до 60 символов)");
    dialog.leftButton = u("Далее");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), name](DialogResponse response, int, StringView text)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showPartyNameInput(*player);
                return;
            }
            const std::string description = ElectionService::sanitizeText(
                Encoding::cp1251Toutf8(std::string(text.data(), text.size())), ElectionService::MAX_DESC_BYTES);
            showPartyConfirm(*player, name, description);
        });
}

void ElectionSystem::showPartyConfirm(IPlayer &player, const std::string &name, const std::string &description)
{
    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u("Партия — взнос");
    dialog.body = u(fmt::format("Партия: {}\n{}\n\nРегистрационный взнос: ${}", name,
                                description.empty() ? "Без описания" : description, ElectionService::PARTY_COST));
    dialog.leftButton = u("Оплатить");
    dialog.rightButton = u("Отмена");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), name, description](DialogResponse response, int, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;
            const PlayerSessionService::Session *session = m_sessionService.get(playerId);
            if (!session)
                return;

            if (m_moneyService.getMoney(playerId) < static_cast<unsigned long long>(ElectionService::PARTY_COST))
            {
                player->sendClientMessage(ERROR_COLOUR,
                                          u(fmt::format("Недостаточно наличных: нужно ${}", ElectionService::PARTY_COST)));
                return;
            }

            // Сначала списание, затем создание; не вышло (гонка по имени/второй
            // партии за время диалогов) — возврат взноса.
            m_moneyService.setMoney(*player, m_moneyService.getMoney(playerId) - ElectionService::PARTY_COST);
            const ElectionService::Party *party =
                m_electionService.createParty(session->accountId, player->getName().to_string(), name, description);
            if (!party)
            {
                m_moneyService.setMoney(*player, m_moneyService.getMoney(playerId) + ElectionService::PARTY_COST);
                player->sendClientMessage(ERROR_COLOUR, u("Не вышло зарегистрировать партию, взнос возвращён"));
                return;
            }
            player->sendClientMessage(INFO_COLOUR, u(fmt::format("Партия «{}» зарегистрирована!", party->name)));
        });
}

// ------------------------------------------------------------------ голосование

void ElectionSystem::showVoteMenu(IPlayer &player)
{
    if (!m_electionService.isActive())
    {
        player.sendClientMessage(INFO_COLOUR, u("Выборы сейчас не проводятся"));
        return;
    }
    const PlayerSessionService::Session *session = m_sessionService.get(player.getID());
    if (!session)
        return;
    if (m_electionService.hasVoted(session->accountId))
    {
        player.sendClientMessage(INFO_COLOUR, u("Вы уже проголосовали"));
        return;
    }
    showPartyList(player, true);
}

void ElectionSystem::confirmVote(IPlayer &player, std::int64_t partyId)
{
    const ElectionService::Party *party = m_electionService.getParty(partyId);
    if (!party)
        return;

    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u("Подтверждение голоса");
    dialog.body = u(fmt::format("Отдать голос за партию «{}» (лидер {})?\nГолос нельзя изменить.", party->name,
                                party->leaderName));
    dialog.leftButton = u("Голосовать");
    dialog.rightButton = u("Назад");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID(), partyId](DialogResponse response, int, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showVoteMenu(*player);
                return;
            }
            const PlayerSessionService::Session *session = m_sessionService.get(playerId);
            if (!session)
                return;

            switch (m_electionService.vote(session->accountId, partyId))
            {
            case ElectionService::VoteStatus::Ok:
                player->sendClientMessage(INFO_COLOUR, u("Ваш голос учтён"));
                break;
            case ElectionService::VoteStatus::AlreadyVoted:
                player->sendClientMessage(ERROR_COLOUR, u("Вы уже проголосовали"));
                break;
            case ElectionService::VoteStatus::NotActive:
                player->sendClientMessage(ERROR_COLOUR, u("Выборы уже завершены"));
                break;
            case ElectionService::VoteStatus::NoParty:
                break;
            }
        });
}

// ------------------------------------------------------------------ дев-меню /edev

void ElectionSystem::showDevMenu(IPlayer &player)
{
    std::string status;
    if (m_electionService.isActive())
        status = fmt::format("Выборы идут, осталось {} мин", m_electionService.remainingSeconds() / 60);
    else
        status = "Выборы не идут";

    std::string body = fmt::format("Статус: {}\nНачать выборы\nЗавершить досрочно\nПартии и голоса", status);

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Выборы — дев-меню");
    dialog.body = u(body);
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;
            switch (listItem)
            {
            case 0:
                showDevMenu(*player);
                break;
            case 1:
                if (m_electionService.isActive())
                    player->sendClientMessage(ERROR_COLOUR, u("Выборы уже идут"));
                else
                    showDevDurationInput(*player);
                break;
            case 2:
                if (!m_electionService.isActive())
                    player->sendClientMessage(ERROR_COLOUR, u("Выборы не идут"));
                else
                    finishElection();
                break;
            case 3:
                for (const auto &party : m_electionService.getParties())
                    player->sendClientMessage(INFO_COLOUR, u(fmt::format("«{}» (лидер {}): {} голосов", party.name,
                                                                         party.leaderName, party.votes)));
                break;
            default:
                break;
            }
        });
}

void ElectionSystem::showDevDurationInput(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u("Старт выборов");
    dialog.body = u("Длительность выборов в минутах (1..1440)");
    dialog.leftButton = u("Начать");
    dialog.rightButton = u("Отмена");

    m_dialogService.showNumberInput(
        player, dialog,
        [this, playerId = player.getID()](DialogResponse response, std::int64_t minutes)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;

            if (minutes < 1 || minutes > 1440)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Некорректная длительность"));
                showDevDurationInput(*player);
                return;
            }
            if (!m_electionService.start(Minutes(minutes)))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Не вышло: выборы уже идут или нет ни одной партии"));
                return;
            }

            m_timerService.cancel(m_finishTimer);
            m_finishTimer = m_timerService.setTimeout(Minutes(minutes), [this] { finishElection(); });
            m_core.getPlayers().sendClientMessageToAll(
                ANNOUNCE_COLOUR, u(fmt::format("Объявлены выборы президента! Голосование в мэриях городов, {} мин",
                                               minutes)));
        });
}
