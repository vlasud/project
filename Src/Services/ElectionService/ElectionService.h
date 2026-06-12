#pragma once

#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "types.hpp"
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class ElectionSystem;

// Выборы президента. Источник правды о партиях, состоянии выборов и голосах.
//
// Цикл:
//  1. Игроки регистрируют партии (пикап у мэрии): название, описание, взнос
//     PARTY_COST наличными (деньги снимает ElectionSystem). Одна партия на
//     аккаунт, имена уникальны.
//  2. Админы объявляют выборы start(длительность) — голоса прошлых выборов
//     чистятся, в мэриях трёх городов активируются урны.
//  3. Игроки голосуют vote() — один голос на АККАУНТ за все выборы.
//  4. По истечении срока (или досрочно) finish(): партия с максимумом голосов
//     побеждает, её лидер автоматически назначается президентом — лидером
//     фракции id 1 (назначение делает ElectionSystem через FactionService,
//     работает и для оффлайн-лидера).
//
// Персист: party / election / election_vote (write-through; срок выборов —
// unix-время, переживает рестарт сервера: система дозапускает таймер).
class ElectionService final : public IService
{
    friend ElectionSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    static constexpr std::int64_t PARTY_COST = 10000000; // взнос за регистрацию партии
    static constexpr std::size_t MAX_PARTIES = 30;
    static constexpr std::size_t MAX_NAME_BYTES = 48;  // utf-8
    static constexpr std::size_t MAX_DESC_BYTES = 128; // utf-8

    struct Party
    {
        std::int64_t id = 0;
        std::string name;        // utf-8
        std::string description; // utf-8
        std::string leaderName;  // ник лидера (копия на момент создания)
        AccountId leader = PlayerSessionService::NO_ACCOUNT;
        std::uint32_t votes = 0; // голоса ТЕКУЩИХ выборов
    };

    // --- партии ---
    const std::vector<Party> &getParties() const;
    const Party *getParty(std::int64_t partyId) const;
    const Party *getPartyByLeader(AccountId accountId) const;
    bool isNameTaken(const std::string &name) const;
    // Имя/описание — utf-8, уже прошедшие sanitizeText. nullptr — дубликат
    // имени/второй партии лидера/лимит/невалидные данные. Взнос снимает
    // вызывающий ДО вызова.
    const Party *createParty(AccountId leader, const std::string &leaderName, const std::string &name,
                             const std::string &description);

    // --- выборы ---
    bool isActive() const;
    std::int64_t remainingSeconds() const; // 0 — не идут/истекли
    bool start(Minutes duration);          // false — уже идут или нет партий
    bool hasVoted(AccountId accountId) const;

    enum class VoteStatus : std::uint8_t
    {
        Ok,
        NotActive,
        AlreadyVoted,
        NoParty,
    };
    VoteStatus vote(AccountId accountId, std::int64_t partyId);

    // Завершить выборы. Победитель — партия с максимумом голосов (ничья —
    // зарегистрированная раньше, с warning в лог); nullptr — голосов не было.
    const Party *finish();

    // Чистка клиентского текста: управляющие символы и \t выбрасываются,
    // края от пробелов, обрезка по границе utf-8 символа.
    static std::string sanitizeText(std::string_view raw, std::size_t maxBytes);

  private:
    // --- вызывается ElectionSystem (загрузка на старте) ---
    void loadParties(std::vector<Party> parties);
    void loadState(bool active, std::int64_t endsAtUnix);
    void loadVotes(std::vector<std::pair<AccountId, std::int64_t>> votes);

    void writeState();
    Party *findParty(std::int64_t partyId);

    std::vector<Party> m_parties;
    std::int64_t m_nextPartyId = 1; // сервер — единственный писатель партий
    bool m_active = false;
    std::int64_t m_endsAtUnix = 0;
    std::unordered_set<AccountId> m_voted;
};
