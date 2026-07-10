#pragma once

#include "Macro.h"
#include "Services/Core/AttachmentService/AttachmentService.h"
#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/NavigationLockService/NavigationLockService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/PortJobService/PortJobService.h"
#include "Services/PortWalletService/PortWalletService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>

// Работа-грузчик в порту (бизнес-фича, НЕ Core), привод PortJobService. Геймплей
// целиком событийный (пикап/чекпоинт/таймер), per-tick работы нет:
//  * пикап порта -> диалог «Начать/Завершить работу» + «Забрать деньги» + «Информация»;
//  * «Начать работу» ставит ПЕРСОНАЛЬНЫЙ чекпоинт на источнике ящиков (корабль);
//  * вход в чекпоинт источника -> ящик прикрепляется в руку СРАЗУ, синхронно (без
//    анимации подъёма и без задержки) + несение (SpecialAction_Carry) +
//    балансировщик назначает точку сброса склада -> персональный чекпоинт на ней;
//  * вход в чекпоинт сброса -> анимация «положить» -> через таймер ящик снят,
//    +1 к счётчику отнесённых за смену -> $PAY_PER_BOX сразу в ПЕРСИСТЕНТНЫЙ
//    кошелёк порта (PortWalletService, write-through в БД) -> снова чекпоинт
//    источника (цикл);
//  * «Завершить работу» деньги НЕ выплачивает — только завершает смену
//    (сбрасывает волатильный счётчик отнесённых), заработок остаётся в кошельке;
//  * «Забрать деньги» (пункт диалога пикапа, доступен всегда) выплачивает весь
//    накопленный кошелёк на руки (PlayerMoneyService) и обнуляет его.
//
// Ровно ОДНА активная цель за раз (персональный чекпоинт держит только текущую),
// нельзя нести два ящика одновременно. Наличные — только через PlayerMoneyService
// (сессионные, НЕ персистятся); накопленный заработок — через PortWalletService
// (персистентный, переживает дисконнект/смерть/краш); клиенту не доверяем.
class PortJobSystem : public BaseSystem, public PlayerSpawnEventHandler
{
  public:
    PortJobSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    // Респавн работающего игрока (единственный путь сюда в смене — через смерть,
    // после которой onPlayerDeath уже откатил фазу к GoToSource): персональный
    // чекпоинт CheckpointService сам НЕ перепоказывается после смерти-респавна —
    // ставим чекпоинт источника заново. Вне смены (фаза NotWorking) — no-op.
    void onPlayerSpawn(IPlayer &player) override;

  private:
    // --- пикап + диалог ---
    void onPickup(IPlayer &player);
    void onToggleWork(IPlayer &player);
    void onStartWork(IPlayer &player);
    void onFinishWork(IPlayer &player);
    // «Забрать деньги»: ре-валидирует сессию/баланс на клике и выплачивает весь
    // кошелёк порта на руки (PlayerMoneyService), обнуляя его. Доступно в любое
    // время (в смене/вне неё, сразу после релога).
    void onWithdrawMoney(IPlayer &player);
    void showInfo(IPlayer &player);

    // Загрузка кошелька порта по старту сессии (serial-guard) — эталон
    // SpawnChoiceSystem::loadChoice.
    void loadWallet(IPlayer &player, const PlayerSessionService::Session &session);

    // --- цикл: источник -> несение -> сброс -> источник ---
    // Вход в чекпоинт источника: ящик прикрепляется СРАЗУ (без задержки) + играет
    // анимация подъёма; несение/чекпоинт сброса включаются по её завершении.
    void onSourceEnter(IPlayer &player);
    // Конец анимации подъёма: включает SpecialAction_Carry и ставит чекпоинт сброса.
    void onLiftFinished(IPlayer &player);
    void onDropEnter(IPlayer &player);
    void onPutdownFinished(IPlayer &player);

    // Смерть в смене: уронить ящик (detach + PortJobService::dropCarry), снять
    // анимацию/таймер/чекпоинт. Смена НЕ завершается — delivered сохраняется,
    // чекпоинт источника вернёт onPlayerSpawn. Вне смены — no-op.
    void onPlayerDeath(IPlayer &player);

    // Снять ящик из руки (если прикреплён) и очистить учёт слота. Bounds-safe.
    void detachBox(IPlayer &player);
    // Полный сброс: снять ящик, остановить анимацию, снять чекпоинт, обнулить
    // волатильное состояние смены И память кошелька порта (БД НЕ трогает —
    // баланс остаётся до явного «Забрать деньги»). Общий teardown для конца
    // сессии/дисконнекта.
    void resetPlayer(IPlayer &player);

    PortJobService &m_portJobService;
    PortWalletService &m_portWalletService;
    PickupService &m_pickupService;
    CheckpointService &m_checkpointService;
    PlayerAnimationService &m_animationService;
    AttachmentService &m_attachmentService;
    PlayerMoneyService &m_moneyService;
    PlayerStateService &m_stateService;
    PlayerHealthService &m_healthService;
    PlayerDialogService &m_dialogService;
    PlayerSessionService &m_sessionService;
    TimerService &m_timers;
    ScreenNoticeService &m_screenNoticeService; // попапы старта смены и сдачи ящика
    MapIconService &m_mapIconService;
    NavigationLockService &m_navLockService; // лок навигации на всю смену грузчика (GPS недоступен)

    int m_pickup = -1; // хэндл пикапа порта

    // Слот AttachmentService текущего прикреплённого ящика (-1 — не прикреплён).
    // Хранится отдельно от PortJobService (сервис — только фаза/балансировка, не
    // клиентские детали attach) — снимается detachBox по playerId.
    std::array<int, MAX_PLAYERS> m_boxSlot{};

    // Хэндл текущего ожидающего таймера укладки (единственный оставшийся переход с
    // задержкой — взятие ящика на источнике теперь синхронно). Отменяется на
    // «Завершить работу»/сбросе — иначе стале-таймер, дозвонившийся уже после
    // рестарта смены, мог бы (по совпадению фазы) продвинуть НОВЫЙ цикл без
    // реального входа в чекпоинт. Практически недостижимо (дистанции чекпоинт<->
    // пикап велики для возврата за 1.0-1.5с, телепорт ловит PlayerLocationService),
    // но отмена — дешёвая и однозначная защита.
    std::array<TimerService::Handle, MAX_PLAYERS> m_pendingTimer{};
};
