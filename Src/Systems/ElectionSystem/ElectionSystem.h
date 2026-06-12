#pragma once

#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/ElectionService/ElectionService.h"
#include "Services/FactionService/FactionService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <cstdint>

// Привод выборов президента:
//  * пикап у мэрии ЛС — меню партий: список/подробности/регистрация
//    (название -> описание -> подтверждение взноса наличными);
//  * урны в мэриях трёх городов — голосование во время выборов;
//  * /edev — дев-меню админа (до системы ролей): статус, старт с длительностью
//    в минутах, досрочное завершение;
//  * финал по таймеру (переживает рестарт: срок в БД, таймер дозапускается):
//    лидер партии-победителя назначается президентом — лидером фракции
//    PRESIDENT_FACTION_ID, даже если он оффлайн.
class ElectionSystem : public BaseSystem
{
  public:
    static constexpr int PRESIDENT_FACTION_ID = 1; // Администрация Президента

    ElectionSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    void load();
    void createPickups();
    void resumeTimer();
    void finishElection();

    // Меню партий (пикап регистрации).
    void showPartyMenu(IPlayer &player);
    void showPartyList(IPlayer &player, bool forVote);
    void showPartyDetails(IPlayer &player, std::int64_t partyId);
    void showPartyNameInput(IPlayer &player);
    void showPartyDescriptionInput(IPlayer &player, const std::string &name);
    void showPartyConfirm(IPlayer &player, const std::string &name, const std::string &description);

    // Голосование (урна).
    void showVoteMenu(IPlayer &player);
    void confirmVote(IPlayer &player, std::int64_t partyId);

    // Дев-меню /edev.
    void showDevMenu(IPlayer &player);
    void showDevDurationInput(IPlayer &player);

    ElectionService &m_electionService;
    FactionService &m_factionService;
    PickupService &m_pickupService;
    PlayerDialogService &m_dialogService;
    PlayerMoneyService &m_moneyService;
    PlayerSessionService &m_sessionService;
    TimerService &m_timerService;

    TimerService::Handle m_finishTimer;
};
