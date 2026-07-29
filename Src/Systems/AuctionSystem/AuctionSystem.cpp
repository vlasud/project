#include "Systems/AuctionSystem/AuctionSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/MoneyFormat/MoneyFormat.h"
#include "Utils/TimeFormat/TimeFormat.h"
#include <cstdlib>
#include <fmt/format.h>
#include <vector>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Как часто проверяем, не дозрел ли лот. Срок торгов — двое суток, поэтому точности
// минуты хватает с запасом, а тик остаётся дешёвым (проход по лотам со ставками).
constexpr Milliseconds AUCTION_TICK{60000};

// Возврат выдаём НЕ в самом колбэке загрузки денег, а следующим шагом: в том же
// прогоне наблюдателей PlayerAuthSystem может применить кэш баланса через setMoney
// и затереть выданное. Задержка снимает зависимость от порядка наблюдателей.
constexpr Milliseconds REFUND_DELAY{2000};

// Потолок ставки живёт в сервисе рядом с minimumBid — иначе они рассогласуются и
// ставка ровно на потолке станет непробиваемой.
constexpr std::int64_t MAX_BID = AuctionService::MAX_BID;

std::string bidStatus(bool leading)
{
    return leading ? "Ваша ставка самая высокая" : "Ваша ставка перебита";
}

} // namespace

AuctionSystem::AuctionSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_auctionService(serviceRegister.getService<AuctionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_moneyPersistService(serviceRegister.getService<PlayerMoneyPersistService>()),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>()),
      m_navLockService(serviceRegister.getService<NavigationLockService>()),
      m_timers(serviceRegister.getService<TimerService>())
{
    serviceRegister.getService<PlayerCommandService>().add(
        "auc", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            showCategories(player);
        },
        {}, "ваши ставки на аукционах", PlayerCommandService::HelpCategory::Economy);

    // Окна торгов — общие: фича со своей точки зовёт openLot и ничего про диалоги
    // не знает.
    m_auctionService.bindWindow(
        [this](IPlayer &player, std::size_t category, int lotId)
        {
            showLotWindow(player, category, lotId);
        });

    // Выдача денег — тоже общая: сервис знает суммы, но наличными распоряжается
    // только привод (офлайновому возврат ложится долгом в БД).
    m_auctionService.bindRefund(
        [this](const AuctionService::Bid &bid, const std::string &reason)
        {
            refund(bid, reason);
        });

    // Возврат ждёт игрока в БД, если тот был офлайн в момент итогов. Цепляемся
    // за ЗАГРУЗКУ ДЕНЕГ, а не за старт сессии: наличные приезжают из БД асинхронно
    // и перетёрли бы выдачу, сделанную раньше них.
    m_moneyPersistService.subscribeMoneyLoaded(
        [this](IPlayer &player, unsigned long long)
        {
            if (m_auctionService.pendingRefund(ownerKeyOf(player.getID())) <= 0)
            {
                return;
            }
            m_timers.setPlayerTimeout(player, REFUND_DELAY,
                                      [this](IPlayer &target)
                                      {
                                          payPendingRefund(target);
                                      });
        });

    // Игрока перебили, пока он был офлайн — однократное уведомление на входе.
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            if (m_auctionService.takeOutbid(std::to_string(session.accountId)))
            {
                player.sendClientMessage(ERROR_COLOUR, u("Вашу ставку на аукционе перебили. Подробнее: /auc"));
            }
        });
}

void AuctionSystem::initialize(IComponentList * /*components*/)
{
    // Торги живут в БД (json фич — дев-контент, в проде read-only). Грузим целиком
    // на старте; до конца загрузки сервис ставки не принимает и итоги не подводит.
    m_auctionService.load();

    // Таймер в initialize (не в конструкторе): компонент таймеров сервису отдаёт
    // TimerSystem. Живёт всю работу сервера, из своего колбэка не отменяется.
    m_auctionTimer = m_timers.setInterval(AUCTION_TICK,
                                          [this]()
                                          {
                                              resolveAuctions();
                                          });
}

// ------------------------------------------------------------------ общее

std::string AuctionSystem::ownerKeyOf(int playerId) const
{
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        return {};
    }
    return std::to_string(session->accountId);
}

IPlayer *AuctionSystem::playerByOwnerKey(const std::string &ownerKey)
{
    if (ownerKey.empty())
    {
        return nullptr;
    }
    const auto accountId = static_cast<PlayerSessionService::AccountId>(std::strtoll(ownerKey.c_str(), nullptr, 10));
    if (accountId == PlayerSessionService::NO_ACCOUNT)
    {
        return nullptr;
    }
    const int playerId = m_sessionService.playerByAccount(accountId);
    return playerId < 0 ? nullptr : m_core.getPlayers().get(playerId);
}

std::string AuctionSystem::lotTitle(std::size_t category, int lotId) const
{
    AuctionService::Lot lot;
    return m_auctionService.lotInfo(category, lotId, lot) ? lot.title : std::string();
}

// ------------------------------------------------------------------ окна торгов по лоту

void AuctionSystem::showLotWindow(IPlayer &player, std::size_t category, int lotId)
{
    const int playerId = player.getID();
    AuctionService::Lot lot;
    if (!m_auctionService.lotInfo(category, lotId, lot))
    {
        return; // лот исчез либо уже не разыгрывается
    }
    const AuctionService::Bid *best = m_auctionService.highestBid(category, lotId);

    std::string body = lot.title + "\n";
    if (!lot.description.empty())
    {
        body += lot.description + "\n";
    }
    body += fmt::format("Стартовая цена: {}\n\n", Money::text(lot.minPrice));
    body += best ? fmt::format("Высшая ставка: {}\n", Money::text(best->amount)) : std::string("Ставок нет\n");
    body += "\nЛот ничей и разыгрывается на аукционе.\nВладельцем станет тот, чья ставка окажется выше.";

    m_dialogService.show(player, makeDialog(DialogStyle_MSGBOX, "Аукцион", body, "Ставки", "Закрыть"),
                         [this, playerId, category, lotId](DialogResponse response, int, StringView)
                         {
                             IPlayer *visitor = m_core.getPlayers().get(playerId);
                             if (!visitor || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             showBidMenu(*visitor, category, lotId);
                         });
}

void AuctionSystem::showBidMenu(IPlayer &player, std::size_t category, int lotId)
{
    const int playerId = player.getID();
    // Пункты видны ВСЕГДА (правило проекта): «Забрать ставку» без ставки не
    // прячется, а объясняется по клику.
    const std::string body = "Информация\nСделать ставку\nЗабрать ставку";

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Ставки", body, "Выбрать", "Назад"),
                         [this, playerId, category, lotId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *visitor = m_core.getPlayers().get(playerId);
                             if (!visitor)
                             {
                                 return;
                             }
                             if (response != DialogResponse_Left)
                             {
                                 showLotWindow(*visitor, category, lotId);
                                 return;
                             }
                             switch (listItem)
                             {
                             case 0:
                                 showLotInfo(*visitor, category, lotId);
                                 break;
                             case 1:
                                 showBidInput(*visitor, category, lotId);
                                 break;
                             case 2:
                                 cancelBid(*visitor, category, lotId);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void AuctionSystem::showLotInfo(IPlayer &player, std::size_t category, int lotId)
{
    const int playerId = player.getID();
    AuctionService::Lot lot;
    if (!m_auctionService.lotInfo(category, lotId, lot))
    {
        return;
    }
    const AuctionService::Bid *best = m_auctionService.highestBid(category, lotId);
    const AuctionService::Bid *own = m_auctionService.bidOf(category, lotId, ownerKeyOf(playerId));
    const std::int64_t endsAt = m_auctionService.endsAt(category, lotId);

    std::string body = lot.title + "\n";
    if (!lot.description.empty())
    {
        body += lot.description + "\n";
    }
    body += fmt::format("Стартовая цена: {}\n\n", Money::text(lot.minPrice));
    body += fmt::format("Ставок: {}\n", m_auctionService.bidCount(category, lotId));
    body += best ? fmt::format("Высшая ставка: {}\n", Money::text(best->amount))
                 : std::string("Высшая ставка: ставок нет\n");
    body += own ? fmt::format("Ваша ставка: {}\n\n", Money::text(own->amount)) : std::string("Ваша ставка: нет\n\n");
    body += endsAt > 0 ? fmt::format("Аукцион завершится: {}\n(осталось {})", TimeFormat::dateTime(endsAt),
                                     TimeFormat::left(endsAt - TimeFormat::nowUnix()))
                       : fmt::format("Аукцион ещё не начат: отсчёт {} суток пойдёт с первой ставки.",
                                     AuctionService::AUCTION_SECONDS / 86400);

    m_dialogService.show(player, makeDialog(DialogStyle_MSGBOX, "Аукцион — информация", body, "Назад", ""),
                         [this, playerId, category, lotId](DialogResponse, int, StringView)
                         {
                             IPlayer *visitor = m_core.getPlayers().get(playerId);
                             if (visitor)
                             {
                                 showBidMenu(*visitor, category, lotId);
                             }
                         });
}

void AuctionSystem::showBidInput(IPlayer &player, std::size_t category, int lotId)
{
    const int playerId = player.getID();
    AuctionService::Lot lot;
    if (!m_auctionService.lotInfo(category, lotId, lot))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот лот больше не разыгрывается"));
        return;
    }
    // Отказываем ДО ввода суммы: набирать цифры, чтобы получить отказ, — впустую.
    // Авторитетная проверка всё равно повторится на приёме ставки.
    if (!m_auctionService.eligible(category, ownerKeyOf(playerId)))
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас уже есть такой объект — второй в одни руки не даётся"));
        return;
    }

    const std::int64_t minimum = m_auctionService.minimumBid(category, lotId);
    if (minimum <= 0)
    {
        // Высшая ставка упёрлась в потолок — перебить нечем. Честно говорим это
        // вместо диалога, который отвергнет любое введённое число.
        player.sendClientMessage(ERROR_COLOUR, u("Ставка уже на максимуме — перебить её нельзя"));
        return;
    }

    std::string body = fmt::format("Минимальная ставка: {}\n\n", Money::text(minimum));
    body += "Деньги списываются сразу. Если вы проиграете аукцион\n";
    body += "или заберёте ставку — они вернутся полностью.\n\nВведите сумму:";

    m_dialogService.show(player, makeDialog(DialogStyle_INPUT, "Сделать ставку", body, "Поставить", "Назад"),
                         [this, playerId, category, lotId](DialogResponse response, int, StringView text)
                         {
                             IPlayer *visitor = m_core.getPlayers().get(playerId);
                             if (!visitor)
                             {
                                 return;
                             }
                             if (response != DialogResponse_Left)
                             {
                                 showBidMenu(*visitor, category, lotId);
                                 return;
                             }
                             const std::string entered(text.data(), text.size());
                             char *end = nullptr;
                             const long long amount = std::strtoll(entered.c_str(), &end, 10);
                             if (entered.empty() || end == entered.c_str() || *end != '\0' || amount <= 0 ||
                                 amount > MAX_BID)
                             {
                                 visitor->sendClientMessage(
                                     ERROR_COLOUR, u(fmt::format("Ставка — целое число от 1 до {}", Money::text(MAX_BID))));
                                 showBidInput(*visitor, category, lotId);
                                 return;
                             }
                             placeBid(*visitor, category, lotId, static_cast<std::int64_t>(amount));
                         });
}

void AuctionSystem::placeBid(IPlayer &player, std::size_t category, int lotId, std::int64_t amount)
{
    const int playerId = player.getID();
    const std::string ownerKey = ownerKeyOf(playerId);
    if (ownerKey.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Ставки доступны только под аккаунтом"));
        return;
    }
    // Ре-валидация на клике: пока висел диалог, лот могли снести, разыграть или
    // перебить ставку.
    AuctionService::Lot lot;
    if (!m_auctionService.lotInfo(category, lotId, lot))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Аукцион уже завершён"));
        return;
    }
    if (!m_auctionService.eligible(category, ownerKey))
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас уже есть такой объект — второй в одни руки не даётся"));
        return;
    }
    const std::int64_t minimum = m_auctionService.minimumBid(category, lotId);
    if (minimum <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Ставка уже на максимуме — перебить её нельзя"));
        return;
    }
    if (amount < minimum)
    {
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Минимальная ставка — {}", Money::text(minimum))));
        showBidInput(player, category, lotId);
        return;
    }
    // Деньги забираем ДО записи ставки: обратный порядок оставил бы ставку без
    // покрытия, если денег не хватило.
    if (!m_moneyService.take(player, static_cast<unsigned long long>(amount)))
    {
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Не хватает денег: нужно {}", Money::text(amount))));
        return;
    }
    std::int64_t refundPrevious = 0;
    std::string outbidOwner;
    if (!m_auctionService.placeBid(category, lotId, ownerKey, amount, TimeFormat::nowUnix(), refundPrevious,
                                   outbidOwner))
    {
        m_moneyService.giveMoney(player, static_cast<unsigned long long>(amount)); // ставка не принята
        player.sendClientMessage(ERROR_COLOUR, u("Ставка не принята"));
        return;
    }
    if (refundPrevious > 0)
    {
        // Прежняя ставка этого же игрока перекрыта новой — старая сумма назад
        // целиком (списаны обе).
        m_moneyService.giveMoney(player, static_cast<unsigned long long>(refundPrevious));
    }
    notifyOutbid(outbidOwner, category, lotId);

    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Ставка {} принята. Аукцион завершится {}", Money::text(amount),
                                           TimeFormat::dateTime(m_auctionService.endsAt(category, lotId)))));
    showLotWindow(player, category, lotId);
}

void AuctionSystem::cancelBid(IPlayer &player, std::size_t category, int lotId)
{
    const std::string ownerKey = ownerKeyOf(player.getID());
    if (ownerKey.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Ставки доступны только под аккаунтом"));
        return;
    }
    const std::int64_t amount = m_auctionService.cancelBid(category, lotId, ownerKey);
    if (amount <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас нет ставки на этот лот"));
        showBidMenu(player, category, lotId);
        return;
    }
    m_auctionService.takeOutbid(ownerKey); // ставки нет — уведомление устарело
    m_moneyService.giveMoney(player, static_cast<unsigned long long>(amount));
    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Ставка отозвана, вам возвращено {}", Money::text(amount))));
    showLotWindow(player, category, lotId);
}

// ------------------------------------------------------------------ /auc

void AuctionSystem::showCategories(IPlayer &player)
{
    const int playerId = player.getID();
    const std::size_t count = m_auctionService.categoryCount();
    if (count == 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Аукционов на сервере нет"));
        return;
    }
    const std::string ownerKey = ownerKeyOf(playerId);

    // Категория показывается ВСЕГДА, даже без ставок (правило проекта). Статус —
    // агрегат: хоть одна перебитая ставка важнее, чем все лидирующие.
    std::string body = "Категория\tСтавки\n";
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::vector<int> lots = m_auctionService.lotsOf(i, ownerKey);
        bool anyOutbid = false;
        for (const int lotId : lots)
        {
            const AuctionService::Bid *best = m_auctionService.highestBid(i, lotId);
            anyOutbid = anyOutbid || !best || best->owner != ownerKey;
        }
        body += fmt::format("{}\t{}\n", m_auctionService.categoryName(i),
                            lots.empty() ? std::string("Ставок нет") : bidStatus(!anyOutbid));
    }
    body.pop_back();

    m_dialogService.show(player,
                         makeDialog(DialogStyle_TABLIST_HEADERS, "Аукционы — ваши ставки", body, "Открыть", "Закрыть"),
                         [this, playerId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *visitor = m_core.getPlayers().get(playerId);
                             if (!visitor || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             if (listItem < 0 || listItem >= static_cast<int>(m_auctionService.categoryCount()))
                             {
                                 return; // индекс от клиента — по живому реестру
                             }
                             showCategory(*visitor, static_cast<std::size_t>(listItem));
                         });
}

void AuctionSystem::showCategory(IPlayer &player, std::size_t category)
{
    const int playerId = player.getID();
    const std::string ownerKey = ownerKeyOf(playerId);
    const std::vector<int> lots = m_auctionService.lotsOf(category, ownerKey);
    if (lots.empty())
    {
        player.sendClientMessage(
            INFO_COLOUR, u(fmt::format("У вас нет ставок в категории «{}»", m_auctionService.categoryName(category))));
        showCategories(player);
        return;
    }
    if (lots.size() == 1)
    {
        showMyLot(player, category, lots.front()); // одна ставка — промежуточный список лишний
        return;
    }

    std::string body = "Лот\tВаша ставка\tСтатус\n";
    for (const int lotId : lots)
    {
        const AuctionService::Bid *own = m_auctionService.bidOf(category, lotId, ownerKey);
        const AuctionService::Bid *best = m_auctionService.highestBid(category, lotId);
        std::string title = lotTitle(category, lotId);
        if (title.empty())
        {
            title = fmt::format("Лот #{}", lotId); // лот уже не разыгрывается — деньги вернёт тик итогов
        }
        body += fmt::format("{}\t{}\t{}\n", title, Money::text(own ? own->amount : 0),
                            bidStatus(best && best->owner == ownerKey));
    }
    body.pop_back();

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_TABLIST_HEADERS, m_auctionService.categoryName(category), body, "Открыть", "Назад"),
        [this, playerId, category, shown = lots](DialogResponse response, int listItem, StringView)
        {
            IPlayer *visitor = m_core.getPlayers().get(playerId);
            if (!visitor)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showCategories(*visitor);
                return;
            }
            if (listItem < 0 || listItem >= static_cast<int>(shown.size()))
            {
                return; // индекс от клиента — по снимку, с которым строился список
            }
            showMyLot(*visitor, category, shown[static_cast<std::size_t>(listItem)]);
        });
}

void AuctionSystem::showMyLot(IPlayer &player, std::size_t category, int lotId)
{
    const int playerId = player.getID();
    const std::string ownerKey = ownerKeyOf(playerId);
    // Снимок берём заново: пока висел прошлый диалог, ставку могли перебить, а
    // аукцион — завершиться.
    const AuctionService::Bid *own = m_auctionService.bidOf(category, lotId, ownerKey);
    if (!own)
    {
        player.sendClientMessage(INFO_COLOUR, u("Этой ставки больше нет"));
        showCategories(player);
        return;
    }
    const AuctionService::Bid *best = m_auctionService.highestBid(category, lotId);
    const std::int64_t endsAt = m_auctionService.endsAt(category, lotId);
    const bool leading = best && best->owner == ownerKey;

    AuctionService::Lot lot;
    const bool alive = m_auctionService.lotInfo(category, lotId, lot);

    std::string body = fmt::format("{}\n\n", alive ? lot.title : fmt::format("Лот #{}", lotId));
    if (alive && !lot.description.empty())
    {
        body += lot.description + "\n";
    }
    body += fmt::format("Ваша ставка: {}\n", Money::text(own->amount));
    body += fmt::format("Высшая ставка: {}{}\n", Money::text(best ? best->amount : own->amount),
                        leading ? " (ваша)" : "");
    body += fmt::format("Всего ставок: {}\n", m_auctionService.bidCount(category, lotId));
    body += fmt::format("Статус: {}\n\n", bidStatus(leading));
    body += endsAt > 0 ? fmt::format("Торги закончатся: {}\n(осталось {})", TimeFormat::dateTime(endsAt),
                                     TimeFormat::left(endsAt - TimeFormat::nowUnix()))
                       : std::string("Срок торгов ещё не запущен");

    m_dialogService.show(player, makeDialog(DialogStyle_MSGBOX, "Ставка", body, "Отметить на GPS", "Назад"),
                         [this, playerId, category, lotId](DialogResponse response, int, StringView)
                         {
                             IPlayer *visitor = m_core.getPlayers().get(playerId);
                             if (!visitor)
                             {
                                 return;
                             }
                             if (response != DialogResponse_Left)
                             {
                                 showCategory(*visitor, category);
                                 return;
                             }
                             markOnGps(*visitor, category, lotId);
                         });
}

void AuctionSystem::markOnGps(IPlayer &player, std::size_t category, int lotId)
{
    const int playerId = player.getID();
    // Лок навигации проверяем на КЛИКЕ (серверный факт): пока висел диалог, игрок
    // мог выйти на смену, где маршрут ведёт работа.
    if (m_navLockService.isLocked(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("GPS недоступен: {}", m_navLockService.lockReason(playerId))));
        return;
    }
    AuctionService::Lot lot;
    if (!m_auctionService.lotInfo(category, lotId, lot))
    {
        player.sendClientMessage(INFO_COLOUR, u("Этот лот больше не разыгрывается"));
        return;
    }

    // Единый чекпоинт-слот через VehicleWaypointService: маркер заменяет прежний
    // GPS/парковочный указатель, вход в чекпоинт гасит его сам.
    const std::string title = lot.title;
    m_waypointService.showGpsFor(player, lot.position,
                                 [title](IPlayer &target)
                                 {
                                     target.sendClientMessage(INFO_COLOUR,
                                                              u(fmt::format("Вы прибыли: {}. GPS отключён", title)));
                                 });
    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("GPS: маршрут до «{}» построен — следуйте за красным чекпоинтом", title)));
}

// ------------------------------------------------------------------ итоги и деньги

void AuctionSystem::resolveAuctions()
{
    const std::vector<AuctionService::Result> results = m_auctionService.resolveDue(TimeFormat::nowUnix());
    for (const AuctionService::Result &result : results)
    {
        const std::string title = lotTitle(result.category, result.lotId);
        if (!result.winner.empty())
        {
            if (IPlayer *winner = playerByOwnerKey(result.winner))
            {
                winner->sendClientMessage(INFO_COLOUR,
                                          u(fmt::format("Аукцион завершён: {} теперь ваш (ставка {})",
                                                        title.empty() ? "лот" : title, Money::text(result.winningAmount))));
            }
        }
        for (const AuctionService::Bid &bid : result.refunds)
        {
            refund(bid, fmt::format("Аукцион ({}) проигран", title.empty() ? "лот снят" : title));
        }
    }
}

void AuctionSystem::refund(const AuctionService::Bid &bid, const std::string &reason)
{
    // Пометка «тебя перебили» больше не актуальна: торги по лоту кончились.
    m_auctionService.takeOutbid(bid.owner);

    IPlayer *loser = playerByOwnerKey(bid.owner);
    if (!loser)
    {
        // Офлайн — наличных нет, кладём долгом в БД до ближайшего входа.
        m_auctionService.addPendingRefund(bid.owner, bid.amount);
        return;
    }
    m_moneyService.giveMoney(*loser, static_cast<unsigned long long>(bid.amount));
    loser->sendClientMessage(INFO_COLOUR,
                             u(fmt::format("{}: ставка {} возвращена", reason, Money::text(bid.amount))));
}

void AuctionSystem::notifyOutbid(const std::string &ownerKey, std::size_t category, int lotId)
{
    if (ownerKey.empty())
    {
        return;
    }
    IPlayer *outbid = playerByOwnerKey(ownerKey);
    if (!outbid)
    {
        m_auctionService.markOutbid(ownerKey); // офлайн — увидит однократно на входе
        return;
    }
    const std::string title = lotTitle(category, lotId);
    outbid->sendClientMessage(ERROR_COLOUR, u(fmt::format("Вашу ставку ({}) перебили. Подробнее: /auc",
                                                          title.empty() ? "аукцион" : title)));
}

void AuctionSystem::payPendingRefund(IPlayer &player)
{
    const std::string ownerKey = ownerKeyOf(player.getID());
    if (ownerKey.empty())
    {
        return;
    }
    const std::int64_t amount = m_auctionService.takePendingRefund(ownerKey);
    if (amount <= 0)
    {
        return;
    }
    m_moneyService.giveMoney(player, static_cast<unsigned long long>(amount));
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Возврат по аукциону: {}", Money::text(amount))));
}
