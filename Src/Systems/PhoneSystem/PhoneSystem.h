#pragma once

#include "Macro.h"
#include "Services/BusinessService/BusinessService.h"
#include "Services/Core/AttachmentService/AttachmentService.h"
#include "Services/Core/AudioService/AudioService.h"
#include "Services/Core/GridService/GridService.h"
#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/FactionService/FactionService.h"
#include "Services/PhoneService/PhoneService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Телефон — привод PhoneService. Бизнес-фича, НЕ Core.
//
//  /c              — меню: скорая / полиция / такси / набрать номер;
//  /c <номер>      — быстрый набор номера (пункт 4 без меню);
//  /pickup         — ответить на входящий звонок;
//  /hangup         — сбросить входящий, отменить свой или завершить разговор;
//  /acceptjob <N>  — работник принимает вызов N.
//
// ТЕЛЕФОН ПОКУПАЕТСЯ в 24/7 и он же ЕСТЬ НОМЕР. Услугу точки система регистрирует
// САМА (BusinessService::addService) — магазин про телефон ничего не знает.
// Покупка: игрок вводит желаемый свободный номер либо оставляет поле пустым, и
// номер подбирается случайно. Повторная покупка МЕНЯЕТ номер (старый освобождается
// тем же UPDATE). Порядок шагов — в Docs/Phone.md, он важен: номер занимается ДО
// денег, и любая неудача после занятия компенсируется откатом записи в БД.
//
// Вызов службы рассылается ТОЛЬКО тем, кто у неё сейчас на смене: список приносит
// сама работа (саморегистрация в PhoneService), телефон про работы не знает. Каждому
// работнику дистанция считается СВОЯ — от него до звонящего.
//
// Принятие забирает вызов у остальных (первый успел — заказ исчез), ставит принявшему
// указатель на место звонка и сообщает звонящему, что помощь выехала.
//
// Разговор игрок<->игрок идёт ОБЫЧНЫМ ЧАТОМ: система подписана на генерик-хук реплики
// (PlayerChatService::subscribeSpeech) и шлёт копию собеседнику. Локальный чат при
// этом не подавляется — стоящие рядом слышат вашу половину разговора.
//
// ВИДИМАЯ ЧАСТЬ ЗВОНКА (телефон в руке, анимации, гудок) — эффекты. Звонящий достаёт
// телефон сразу, адресат — когда ответил; конец звонка убирает телефон обратной
// анимацией. Гудок и звонок — короткий звук, повторяемый таймером, поэтому «выключить»
// его — это просто остановить повтор.
class PhoneSystem : public BaseSystem
{
  public:
    // Цена телефона в 24/7. Телефон и есть номер, поэтому повторная покупка — это
    // смена номера за те же деньги.
    static constexpr std::int64_t PHONE_PRICE = 1000;

    PhoneSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    // --- команды ---
    void onCallCommand(IPlayer &player, const std::string &argument);
    void showCallMenu(IPlayer &player);
    void showDialNumberDialog(IPlayer &player);
    void onAcceptJobCommand(IPlayer &worker, int orderNumber);
    void onPickupCommand(IPlayer &player);
    void onHangupCommand(IPlayer &player);

    // --- вызовы служб ---
    void placeServiceCall(IPlayer &player, PhoneService::Service service);
    void dialNumber(IPlayer &player, std::int64_t phone);

    // --- счёт телефона ---
    // Списать за интервал разговора; false — на счету не хватило (звонок обрывается).
    bool chargeCall(IPlayer &payer);
    // Записать счёт в БД. Пишется ПОСЛЕ изменения кэша, порядок записей одного
    // аккаунта держится ключом очереди — иначе списание и пополнение могли бы
    // разъехаться и последним лечь устаревшее значение.
    void persistBalance(PlayerSessionService::AccountId accountId, std::int64_t balance);
    // --- пополнение счёта (услуга 24/7) ---
    void beginTopUp(IPlayer &player, int businessId);
    void showTopUpDialog(IPlayer &player, int businessId, const std::string &hint);
    void topUp(IPlayer &player, int businessId, std::int64_t amount);

    // --- покупка телефона (услуга 24/7) ---
    void beginPurchase(IPlayer &player, int businessId);
    // hint — причина отказа предыдущей попытки (utf-8); пусто — первый показ.
    void showNumberDialog(IPlayer &player, int businessId, const std::string &hint);
    // desired == 0 — сервер подберёт случайный свободный номер.
    void claimNumber(IPlayer &player, int businessId, std::int64_t desired);
    // Откат занятого номера, если покупка не состоялась: вернуть аккаунту старый
    // номер, а при неудаче (его успели занять) — обнулить. Старый номер вернуть не
    // удалось — гасит и кэш игрока, иначе он остался бы с номером, которого в БД
    // уже нет (и который мог достаться другому).
    void rollbackNumber(PlayerSessionService::AccountId accountId, std::int64_t claimed, std::int64_t previous,
                        int playerId, std::uint32_t serial);
    void completePurchase(IPlayer &player, int businessId, std::int64_t phone);

    // --- эффекты звонка: телефон в руке, анимации, гудок ---
    // Достать телефон: прикрепить модель к правой руке и проиграть phone_in, а по её
    // окончании — зацикленный phone_talk. Повторный вызов переиспользует уже
    // прикреплённый телефон (второй в руке не появится).
    void raisePhone(IPlayer &player);
    // Убрать телефон: обратная анимация, модель снимается в её КОНЦЕ.
    void lowerPhone(IPlayer &player);
    void startRing(IPlayer &player); // гудок/звонок: первый звук сразу, дальше повтором
    void stopRing(int playerId);
    // Снять эффекты звонка у себя.
    void endCallEffects(int playerId);
    // То же собеседнику, но только если слот занят ТОЙ ЖЕ сессией: иначе погасили бы
    // звонок постороннему игроку, которому достался переиспользованный playerId.
    void endPeerEffects(int peerId, std::uint32_t peerSerial);
    // Полный сброс визуального состояния слота (конец сессии): без анимаций.
    void clearPhoneProps(IPlayer &player);

    // --- звонки игрок<->игрок ---
    bool requirePhone(IPlayer &player); // false — телефона нет, отказ уже отправлен
    void tickCalls();
    // Реплика игрока из чата. true — игрок сейчас говорит по телефону, и реплику
    // забрал телефон: рассылку делает он сам, обычный чат её не показывает.
    bool onSpeech(int playerId, StringView message);
    // Снять звонок игрока и сообщить собеседнику причину (utf-8).
    void dropCall(int playerId, const std::string &reason);
    // Сообщение собеседнику с проверкой, что слот занят ТОЙ ЖЕ сессией.
    void notifyPeer(int peerId, std::uint32_t peerSerial, const std::string &message);

    // --- лайфцикл сессии ---
    void loadPhone(IPlayer &player, const PlayerSessionService::Session &session);
    void onSessionEnd(IPlayer &player);

    PhoneService &m_phoneService;
    PlayerSessionService &m_sessionService;
    PlayerLocationService &m_locationService;
    PlayerHealthService &m_healthService;
    FactionService &m_factionService;
    VehicleWaypointService &m_waypointService;
    PlayerDialogService &m_dialogService;
    AudioService &m_audioService;
    BusinessService &m_businessService;
    PlayerMoneyService &m_moneyService;
    ScreenNoticeService &m_noticeService;
    TimerService &m_timers;
    PlayerAnimationService &m_animationService;
    AttachmentService &m_attachments;
    PlayerStateService &m_stateService; // анимацию телефона крутим только пешему
    GridService &m_gridService;         // кто рядом слышит вашу половину разговора

    // Буфер слушателей проксимити-рассылки: переиспользуется, чтобы не аллоцировать
    // на каждую реплику.
    std::vector<GridService::Result> m_listeners;

    // Слот прикреплённого телефона; -1 — телефона в руке нет. Держится и во время
    // обратной анимации: новый звонок в этот момент переиспользует ту же модель.
    std::array<int, MAX_PLAYERS> m_phoneSlot;
    // Телефон логически «поднят». Отличается от m_phoneSlot: пока идёт phone_out,
    // модель ещё прикреплена, но телефон уже убран.
    std::array<bool, MAX_PLAYERS> m_phoneUp{};
    // Повтор гудка/звонка и переход между анимациями телефона. Пер-плеерные:
    // отменяются сами при выходе игрока.
    std::array<TimerService::Handle, MAX_PLAYERS> m_ringTimer{};
    std::array<TimerService::Handle, MAX_PLAYERS> m_animTimer{};

    // Один общий тик дозвонов на весь сервер: звонков единицы, за-игроковых
    // таймеров не плодим. НИКОГДА не отменяется (в том числе из своего колбэка).
    TimerService::Handle m_callTimer;
};
