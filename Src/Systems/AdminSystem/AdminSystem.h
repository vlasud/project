#pragma once

#include "Services/AdminService/AdminService.h"
#include "Services/BanService/BanService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <cstdint>
#include <string>

// Привод админ-системы (уровни, /alogin, выдача/регистрация, /a, /kick, /ban).
//  * персист через сессию: на старте грузит level+хеш в AdminService (serial-
//    guard), на конце — reset слота;
//  * права команд декларируются PermissionSpec и гейтятся getEffectiveLevel
//    (резолвер в PlayerCommandSystem) — 0 до успешного /alogin в этой сессии;
//  * /alogin — вход в админку (верификация пароля на воркере, анти-брутфорс);
//    уровень 6 («Разработчик») авто-логинится на старте сессии без пароля;
//  * /setadmin — выдача/снятие уровня (порог 5, диапазон 0..5 — уровень 6 командой
//    не выдаётся); при выдаче нерегистрированной цели — диалоговая регистрация
//    админ-пароля;
//  * /a — админ-чат всем залогиненным админам; /kick — кик игрока;
//  * /ban — бан аккаунта на дни (уровень 3): запись в BanService + кик, проверка
//    активности бана — на логине (PlayerAuthSystem).
class AdminSystem : public BaseSystem
{
  public:
    AdminSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    void loadAdmin(IPlayer &player, const PlayerSessionService::Session &session);

    // --- команды ---
    void cmdLogin(IPlayer &player, StringView password);
    void cmdSetAdmin(IPlayer &actor, int targetId, int level);
    void cmdAdminChat(IPlayer &player, StringView rawText);
    // /an — личный ответ администрации игроку (обезличенно, без имени/уровня админа).
    void cmdAdminNotice(IPlayer &actor, int targetId, StringView rawText);
    void cmdKick(IPlayer &actor, int targetId, StringView rawReason);
    void cmdBan(IPlayer &actor, int targetId, int days, StringView rawReason);
    // Парный админ-телепорт (ур.1): /goto — к игроку, /gethere — игрока к себе.
    void cmdGoto(IPlayer &actor, int targetId);
    void cmdGetHere(IPlayer &actor, int targetId);
    // /ahelp — диалог со списком доступных игроку админ-команд (по эфф. уровню).
    void cmdAdminHelp(IPlayer &player);

    // --- регистрация админа (диалоги пароля для цели /setadmin) ---
    void startRegistration(IPlayer &target, int level);
    void showRegisterPasswordDialog(IPlayer &target);
    void showRegisterConfirmDialog(IPlayer &target);
    void finishRegistration(IPlayer &target);

    // [A]-канал всем залогиненным админам + файл-лог.
    void logAdminAction(const std::string &utf8Line);

    AdminService &m_adminService;
    BanService &m_banService;
    PlayerSessionService &m_sessionService;
    PlayerDialogService &m_dialogService;
    PlayerCommandService &m_commandService; // нужен в /ahelp для перечисления доступных команд
    PlayerLocationService &m_locationService; // легитимный для анти-чита перенос (/goto, /gethere)

    // Пер-цель состояние регистрации админа (шаги диалога пароля). serial —
    // сессии ЦЕЛИ на момент старта: колбэк диалога/финал сверяет его (в слот мог
    // сесть другой игрок).
    struct PendingRegistration
    {
        bool active = false;
        int level = 0;
        std::string firstPassword;
        std::uint32_t serial = 0;
    };
    std::array<PendingRegistration, MAX_PLAYERS> m_pending;
};
