#pragma once

#include "Macro.h"
#include "Services/AdminService/AdminService.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>

// Реакция на журнал нарушений: детекторы (здоровье, анимации, урон) пишут в
// AntiCheatService, эта система решает, что делать с игроком. Текущая политика —
// кик при достижении порога нарушений за скользящее окно. Кик — единственное
// действие, которое хакнутый клиент не может проигнорировать.
//
// Владеет жизненным циклом записей: чистит журнал при отключении игрока.
class AntiCheatSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    AntiCheatSystem(ICore &core, const ServiceRegister &serviceRegister);

    // Телеметрия клиента: версия и сборка пишутся в журнал на входе — по ним видно,
    // с какого клиента приходят нарушения (ядро логирует подключение без версии).
    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    void onViolation(int playerId, AntiCheatService::ViolationType type, const AntiCheatService::PlayerRecord &record);

    // Канал [AC]: строка о нарушении всем залогиненным админам онлайн.
    void notifyAdmins(IPlayer &player, AntiCheatService::ViolationType type) const;

    // Есть ли онлайн хоть один залогиненный админ — от этого зависит агрессивный
    // режим начисления счёта.
    bool anyAdminOnline() const;

    // Показывает игроку, за что его отключают, и откладывает сам кик: kick() рвёт
    // соединение сразу, и отправленный в том же кадре диалог до клиента не дойдёт.
    void kickWithNotice(IPlayer &player, const AntiCheatService::PlayerRecord &record);

    // Личный множитель: загрузка на старте сессии и подъём после кика (write-through
    // в БД — следующая сессия пойманного начинается с меньшей терпимостью).
    void loadProfile(IPlayer &player, const PlayerSessionService::Session &session);
    void raiseMultiplier(IPlayer &player);

    AntiCheatService &m_antiCheatService;
    AdminService &m_adminService; // кому слать [AC] — только доказавшим пароль
    PlayerDialogService &m_dialogService;
    TimerService &m_timerService;
    PlayerSessionService &m_sessionService; // аккаунт для персиста множителя

    // Кик уже назначен: новые нарушения за время показа диалога не должны плодить
    // ни повторные окна, ни повторные таймеры.
    std::array<bool, MAX_PLAYERS> m_kickPending{};
};
