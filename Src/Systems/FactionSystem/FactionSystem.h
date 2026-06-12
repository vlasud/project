#pragma once

#include "Services/BankService/BankService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/FactionService/FactionService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <cstdint>
#include <optional>

// Привод базовой системы фракций:
//  * на старте грузит ранги и бюджеты из БД в FactionService;
//  * по старту сессии подтягивает членство аккаунта (с serial-guard);
//  * /faction — игроку информация о фракции, лидеру — меню организации:
//    ранги (имя/доступ/common-права/удаление) и приказ о выплате зарплат
//    (сумма зарплат списывается с бюджета, чеки уходят на банковские счета
//    членов, включая оффлайн);
//  * /invite (с вводом персональной зарплаты) и /setsalary — право Invite,
//    /uninvite — право Fire, /setrank — лидер;
//  * /fdev — единое дев-меню (до системы ролей): фракции, принять/лидер/
//    исключить, бюджет.
class FactionSystem : public BaseSystem
{
  public:
    FactionSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    void loadCatalog();
    void loadMembership(IPlayer &player, const PlayerSessionService::Session &session);

    // Команды лидера по людям.
    void inviteMember(IPlayer &leader, int targetId);
    void showInviteSalaryInput(IPlayer &leader, int targetId, std::uint32_t targetSerial);
    void uninviteMember(IPlayer &leader, int targetId);
    void showSetRankDialog(IPlayer &leader, int targetId);
    void showSetSalaryDialog(IPlayer &leader, int targetId);

    // Лидерский UI. Каждый колбэк перепроверяет лидерство — лидера могли
    // снять, пока диалог был открыт.
    void showFactionInfo(IPlayer &player);
    void showLeaderMenu(IPlayer &player); // корневое меню организации
    void showRanksMenu(IPlayer &player);
    void showRankMenu(IPlayer &player, std::int64_t rankId);
    void showRankNameInput(IPlayer &player, std::int64_t rankId); // rankId 0 — создание
    void showRankScopeMenu(IPlayer &player, std::int64_t rankId); // подопечные организации ранга
    void showRankDeleteConfirm(IPlayer &player, std::int64_t rankId);

    // Приказ о выплате зарплат: подтверждение -> сумма зарплат из БД ->
    // списание бюджета -> чеки на счета членов (BankService).
    void confirmPayOrder(IPlayer &player);
    void executePayOrder(IPlayer &player);

    // Меню куратора /gov: подопечные фракции -> назначить/снять лидера.
    // Каждый колбэк перепроверяет canManage — доступ могли срезать.
    void showGovMenu(IPlayer &player);
    void showGovFactionMenu(IPlayer &player, int factionId);
    void showGovAppointInput(IPlayer &player, int factionId);
    void showGovAppointSalaryInput(IPlayer &player, int factionId, int targetId, std::uint32_t targetSerial);
    void govDismissLeader(IPlayer &player, int factionId);

    // Дев-меню /fdev (одна команда — диалоги вместо россыпи команд).
    enum class DevAction
    {
        Invite,
        MakeLeader,
    };
    void showDevMenu(IPlayer &player);
    void showDevTargetInput(IPlayer &player, DevAction action);
    void showDevFactionPick(IPlayer &player, DevAction action, int targetId, std::uint32_t targetSerial);
    void showDevKickInput(IPlayer &player);
    void showDevBudgetPick(IPlayer &player);
    void showDevBudgetInput(IPlayer &player, int factionId);
    void listFactions(IPlayer &player);

    // Фракция игрока, если он её лидер; nullptr — не лидер (с сообщением).
    const FactionService::Faction *leaderFaction(IPlayer &player);
    // Фракция игрока, если у него есть биты mask; nullptr — нет (с сообщением).
    const FactionService::Faction *permittedFaction(IPlayer &player, FactionService::PermissionMask mask);

    FactionService &m_factionService;
    PlayerSessionService &m_sessionService;
    PlayerDialogService &m_dialogService;
    BankService &m_bankService;
};
