#pragma once

#include "Services/BankService/BankService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/FactionService/FactionService.h"
#include "Services/PlayerPersonalSkinService/PlayerPersonalSkinService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/Core/PlayerSkinService/PlayerSkinService.h"
#include "Services/PlayerSpawnService/PlayerSpawnService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <cstdint>
#include <functional>
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

    // Базы организаций (зарегистрированные конкретными фракциями): пикапы
    // дверей входа/выхода + телепорт. Вход — только членам.
    void createBasePickups();
    void enterBase(IPlayer &player, int factionId, const Vector3 &target, float angle);
    void exitBase(IPlayer &player, int factionId, const Vector3 &target, float angle);

    // Спавн: член фракции с точкой спавна появляется на ней, остальные — на
    // гражданском дефолте. Применяется на событии членства.
    void applyFactionSpawn(IPlayer &player, int factionId);
    // Цвет организации на нике и маркере миникарты (вне фракции — гражданский).
    void applyFactionColour(IPlayer &player, int factionId);
    // Скин члена организации: при входе/появлении в сети — скин организации
    // (сохранённый или первый из пула; запомнив прежний гражданский скин), при
    // выходе — возврат прежнего гражданского. Член всегда в скине организации,
    // пока состоит. Единственная точка применения — событие членства.
    void applyFactionSkin(IPlayer &player, int oldFactionId, int newFactionId);
    // Рация /r: сообщение всем членам организации.
    void radioChat(IPlayer &player, StringView rawText);
    // Смена скина из пула организации (право PERM_SKIN).
    void showSkinDialog(IPlayer &player);

    // Команды лидера по людям.
    void inviteMember(IPlayer &leader, int targetId);
    // Проверка цели найма (право Invite, цель онлайн и вне фракции). false —
    // отказ с сообщением; общая для /invite и кнопки «Нанять сотрудника».
    bool validateHireTarget(IPlayer &leader, int targetId);
    // Найм — единая цепочка: проверка цели -> выбор ранга -> ввод зарплаты ->
    // setMember. id вводится только в showHireInput (точка с ручным id), либо
    // приходит из /invite. Каждый шаг тащит targetSerial: в слот мог сесть
    // другой игрок, пока диалоги открыты.
    void showHireInput(IPlayer &leader);
    void showHireRankPick(IPlayer &leader, int targetId, std::uint32_t targetSerial);
    void showHireSalaryInput(IPlayer &leader, int targetId, std::uint32_t targetSerial, std::int64_t rankId);
    void uninviteMember(IPlayer &leader, int targetId);
    // onClose (если задан) вызывается по завершении/отмене диалога — карточка
    // сотрудника передаёт сюда возврат к себе, чтобы не было тупика; команды
    // /setrank, /setsalary вызывают без него (диалог просто закрывается).
    void showSetRankDialog(IPlayer &leader, int targetId, std::function<void(IPlayer &)> onClose = {});
    void showSetSalaryDialog(IPlayer &leader, int targetId, std::function<void(IPlayer &)> onClose = {});

    // Меню «Сотрудники»: список членов онлайн -> карточка сотрудника (ранг/
    // зарплата/увольнение) без ручного ввода id (выбран из списка). Каждый
    // колбэк перепроверяет лидерство и serial выбранного.
    void showMembersMenu(IPlayer &player);
    void showMemberCard(IPlayer &player, int targetId, std::uint32_t targetSerial);
    // «Мой ранг»: смена ранга на самого себя (та же логика, что setMemberRank).
    void showMyRankMenu(IPlayer &player);
    // Пресеты прав при создании ранга (рядовой/офицер/заместитель/вручную).
    void showRankPresetMenu(IPlayer &player, std::int64_t rankId);

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

    // Меню куратора /gov: подопечные фракции -> назначить/снять лидера/уволить
    // сотрудника. Каждый колбэк перепроверяет canManage — доступ могли срезать.
    void showGovMenu(IPlayer &player);
    void showGovFactionMenu(IPlayer &player, int factionId);
    void showGovAppointInput(IPlayer &player, int factionId);
    void showGovAppointSalaryInput(IPlayer &player, int factionId, int targetId, std::uint32_t targetSerial);
    void govDismissLeader(IPlayer &player, int factionId);
    // Увольнение куратором: список членов подопечной ОНЛАЙН (без ручного ввода
    // id) -> подтверждение -> removeMember (полное исключение). Куратор выше
    // внутрифракционного запрета — может уволить ЛЮБОГО, включая лидера. Каждый
    // колбэк перепроверяет canManage и serial выбранного.
    void showGovMembersMenu(IPlayer &player, int factionId);
    void showGovDismissConfirm(IPlayer &player, int factionId, int targetId, std::uint32_t targetSerial);

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
    // Пикер фракций (только с заданной точкой спавна) -> телепорт на её спавн.
    void showDevSpawnTeleportPick(IPlayer &player);
    void listFactions(IPlayer &player);

    // Фракция игрока, если он её лидер; nullptr — не лидер (с сообщением).
    const FactionService::Faction *leaderFaction(IPlayer &player);
    // Фракция игрока, если у него есть биты mask; nullptr — нет (с сообщением).
    const FactionService::Faction *permittedFaction(IPlayer &player, FactionService::PermissionMask mask);

    FactionService &m_factionService;
    PlayerSessionService &m_sessionService;
    PlayerDialogService &m_dialogService;
    BankService &m_bankService;
    PickupService &m_pickupService;
    PlayerLocationService &m_locationService;
    PlayerSpawnService &m_spawnService;
    PlayerSkinService &m_skinService;
    // Источник правды о ЛИЧНОМ (гражданском) скине аккаунта — в него член
    // возвращается при увольнении. Персист — PlayerPersonalSkinService/System.
    PlayerPersonalSkinService &m_personalSkinService;
};
