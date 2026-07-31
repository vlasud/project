#pragma once

#include "Macro.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/FriendService/FriendService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Друзья — привод FriendService. Бизнес-фича, НЕ Core.
//
//  /friend        — меню: мои друзья / добавить в друзья;
//  /friendaccept  — принять входящую заявку;
//  /friendcancel  — отклонить входящую заявку.
//
// Дружба ВЗАИМНАЯ и хранится двумя строками player_friend (a->b и b->a): список
// друзей аккаунта читается обычным SELECT, а удаление снимает обе строки.
//
// Заявки живут только в памяти (как звонок в телефоне) и адресуются КОНКРЕТНОМУ
// игроку онлайн. Их может висеть несколько сразу, поэтому отвечают на них по id
// отправителя: /friendaccept 22. Приём проверяет серию сессии отправителя — пока
// заявка висела, он мог выйти, а слот playerId — достаться другому.
//
// Смысл дружбы сейчас — видеть вход друга в игру: на старте сессии друзьям онлайн
// уходит сообщение. Список друзей открывается СВЕЖИМ запросом в БД, а не из кэша:
// у оффлайн-друга «последний раз в сети» иначе остался бы снимком времени входа.
class FriendSystem : public BaseSystem
{
  public:
    FriendSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    // --- команды ---
    void showMenu(IPlayer &player);
    void onAcceptCommand(IPlayer &player, int fromId);
    void onCancelCommand(IPlayer &player, int fromId);

    // --- мои друзья ---
    // Тянет свежий список из БД и показывает диалог (async, serial-guard).
    void showFriendList(IPlayer &player);
    void presentFriendList(IPlayer &player, const std::vector<FriendService::Friend> &friends);
    void showFriendActions(IPlayer &player, FriendService::AccountId accountId, const std::string &name);
    void removeFriend(IPlayer &player, FriendService::AccountId accountId, const std::string &name);

    // --- добавление ---
    void showAddDialog(IPlayer &player);
    void sendRequest(IPlayer &sender, int targetId);

    // --- лайфцикл сессии ---
    void loadFriends(IPlayer &player, const PlayerSessionService::Session &session);
    // Сообщить друзьям онлайн, что игрок зашёл.
    void announceOnline(IPlayer &player);
    void touchLastSeen(const PlayerSessionService::Session &session);
    void onSessionEnd(IPlayer &player);
    void tickRequests();

    FriendService &m_friendService;
    PlayerSessionService &m_sessionService;
    PlayerDialogService &m_dialogService;
    TimerService &m_timers;

    TimerService::Handle m_requestTimer;
};
