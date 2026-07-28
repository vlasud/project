#pragma once

#include "Macro.h"
#include "Services/AuctionService/AuctionService.h"
#include "Services/Core/NavigationLockService/NavigationLockService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/PlayerMoneyPersistService/PlayerMoneyPersistService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <cstddef>
#include <string>

// Аукционы — ЕДИНЫЙ привод торгов (бизнес-фича, НЕ Core). Здесь всё, что для
// всех категорий одинаково:
//
//  * ОКНА ТОРГОВ по лоту (их открывает фича со своей точки через
//    AuctionService::openLot): описание + высшая ставка, подменю «Ставки» —
//    информация / сделать ставку / забрать ставку;
//  * /auc — справка по ВСЕМ ставкам игрока с «отметить на GPS»;
//  * тик подведения итогов, выдача возвратов и уведомления «вас перебили».
//
// Персист торгов — В БД, и его ведёт сам AuctionService (write-through, как
// FamilyService): json-файлы фич — дев-контент, в проде они read-only.
//
// Деньги трогает ТОЛЬКО этот привод: сервис знает суммы, но не умеет их списывать
// и выдавать. Так путь денег один на все аукционы — второй ветки не появляется.
//
// Окно /auc СПРАВОЧНОЕ: ставку оттуда ни сделать, ни забрать — это делается у
// самого лота, куда ведёт GPS-маркер (как заработок работ забирается у пикапа
// работы, см. Docs/JobWallet.md).
class AuctionSystem : public BaseSystem
{
  public:
    AuctionSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    // --- окна торгов по лоту (общие для всех категорий) ---
    void showLotWindow(IPlayer &player, std::size_t category, int lotId);
    void showBidMenu(IPlayer &player, std::size_t category, int lotId);
    void showLotInfo(IPlayer &player, std::size_t category, int lotId);
    void showBidInput(IPlayer &player, std::size_t category, int lotId);
    void placeBid(IPlayer &player, std::size_t category, int lotId, std::int64_t amount);
    void cancelBid(IPlayer &player, std::size_t category, int lotId);

    // --- /auc ---
    void showCategories(IPlayer &player);
    void showCategory(IPlayer &player, std::size_t category);
    void showMyLot(IPlayer &player, std::size_t category, int lotId);
    void markOnGps(IPlayer &player, std::size_t category, int lotId);

    // --- итоги, деньги, уведомления ---
    void resolveAuctions();
    void payPendingRefund(IPlayer &player);
    // Сообщить, что ставку перебили: онлайн — в чат, офлайн — пометкой до входа.
    void notifyOutbid(const std::string &ownerKey, std::size_t category, int lotId);
    // Вернуть деньги владельцу ставки: онлайн — на руки, офлайн — долгом в БД.
    void refund(const AuctionService::Bid &bid, const std::string &reason);

    // Ключ аккаунта текущей сессии ("" — не залогинен).
    std::string ownerKeyOf(int playerId) const;
    IPlayer *playerByOwnerKey(const std::string &ownerKey);
    // Заголовок лота для сообщений; "" — лота уже нет.
    std::string lotTitle(std::size_t category, int lotId) const;

    AuctionService &m_auctionService;
    PlayerDialogService &m_dialogService;
    PlayerSessionService &m_sessionService;
    PlayerMoneyService &m_moneyService;
    PlayerMoneyPersistService &m_moneyPersistService;
    VehicleWaypointService &m_waypointService;
    NavigationLockService &m_navLockService;
    TimerService &m_timers;

    TimerService::Handle m_auctionTimer;
};
