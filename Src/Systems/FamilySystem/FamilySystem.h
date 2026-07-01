#pragma once

#include "Services/Core/PlayerChatService/PlayerChatService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/HouseService/HouseService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <cstdint>
#include <vector>

// Привод бизнес-фичи «Семьи»:
//  * на старте грузит ВСЕ семьи и их членов из БД в FamilyService;
//  * по старту/концу сессии резолвит/чистит онлайн-членство игрока;
//  * /family — одна команда, открывающая меню по состоянию игрока (создать /
//    состав+пригласить+выйти+распустить);
//  * /f — семейный чат всем онлайн-членам своей семьи.
//
// Всё членство и владелец — серверная правда из FamilyService (клиенту/старому
// диалогу не доверяем): пригласить может только владелец, согласие приглашённого
// перепроверяет состояние на момент клика.
//
// ГЕЙТ создания: создать семью можно ТОЛЬКО при наличии дома в собственности
// (HouseService::ownsHouse по серверному accountId). Проверка серверная, перед
// FamilyService::createFamily; ключ владельца формируется как в HouseSystem —
// std::to_string(accountId).
class FamilySystem : public BaseSystem
{
  public:
    FamilySystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    void loadAll(); // загрузка семей и членов из БД на старте

    // Семейный чат /f: рассылка всем онлайн-членам своей семьи.
    void familyChat(IPlayer &player, StringView rawText);

    // Меню /family: динамический вектор действий по состоянию игрока.
    void showMenu(IPlayer &player);
    void showCreateInput(IPlayer &player);
    void showRoster(IPlayer &player);              // MSGBOX состава семьи
    void showInviteInput(IPlayer &player);         // только владелец
    void showInviteConfirm(IPlayer &invited, int inviterId, int familyId, std::uint32_t inviterSerial);
    void showLeaveConfirm(IPlayer &player);
    void showDisbandConfirm(IPlayer &player);      // только владелец
    void showFamilyVehicles(IPlayer &player);      // только владелец: список расшаренных машин
    // Под-диалог подтверждения «Забрать» машину dbId из общего пользования семьи.
    void showTakeVehicleConfirm(IPlayer &player, long long dbId);

    // Действия меню (порядок зависит от состояния — диспетчер ведём по вектору,
    // не по магическим индексам).
    enum class Action
    {
        Create,
        Roster,
        Invite,
        Vehicles,
        Leave,
        Disband,
    };
    // Собрать доступные игроку действия в порядке показа.
    std::vector<Action> buildActions(int playerId) const;

    FamilyService &m_familyService;
    PlayerSessionService &m_sessionService;
    PlayerDialogService &m_dialogService;
    PlayerChatService &m_chatService;
    HouseService &m_houseService; // гейт создания семьи: нужен дом в собственности
    ParkedVehicleService &m_parkedService; // семейный парк (список расшаренных / забрать = снять шеринг)
};
