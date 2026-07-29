#include "Services/AuctionService/AuctionService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <algorithm>
#include <cstdlib>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <utility>

namespace
{
const std::string EMPTY_NAME;
} // namespace

// ------------------------------------------------------------------ реестр категорий

void AuctionService::registerCategory(CategoryDef def)
{
    if (def.key.empty() || def.name.empty() || !def.info || !def.eligible || !def.award || !def.ready)
    {
        LogManager::log(Error, "AuctionService: категория аукциона '" + def.name + "' зарегистрирована неполно");
        return;
    }
    if (categoryByKey(def.key) >= 0)
    {
        LogManager::log(Error, "AuctionService: ключ категории '" + def.key + "' занят, регистрация пропущена");
        return; // одинаковые ключи слили бы торги двух фич в один набор лотов
    }
    m_categories.push_back(Category{std::move(def)});
}

std::size_t AuctionService::categoryCount() const
{
    return m_categories.size();
}

bool AuctionService::validCategory(std::size_t category) const
{
    return category < m_categories.size();
}

const std::string &AuctionService::categoryName(std::size_t category) const
{
    return validCategory(category) ? m_categories[category].def.name : EMPTY_NAME;
}

int AuctionService::categoryByKey(const std::string &key) const
{
    for (std::size_t i = 0; i < m_categories.size(); ++i)
    {
        if (m_categories[i].def.key == key)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool AuctionService::lotInfo(std::size_t category, int lotId, Lot &out) const
{
    return validCategory(category) && m_categories[category].def.info(lotId, out);
}

bool AuctionService::eligible(std::size_t category, const std::string &ownerKey) const
{
    return validCategory(category) && !ownerKey.empty() && m_categories[category].def.eligible(ownerKey);
}

void AuctionService::bindWindow(WindowHandler handler)
{
    m_window = std::move(handler);
}

void AuctionService::bindRefund(RefundHandler handler)
{
    m_refund = std::move(handler);
}

void AuctionService::openLot(IPlayer &player, std::size_t category, int lotId) const
{
    if (m_window && validCategory(category))
    {
        m_window(player, category, lotId);
    }
}

// ------------------------------------------------------------------ запрос торгов

const AuctionService::LotAuction *AuctionService::find(std::size_t category, int lotId) const
{
    if (!validCategory(category))
    {
        return nullptr;
    }
    const auto byCategory = m_auctions.find(m_categories[category].def.key);
    if (byCategory == m_auctions.end())
    {
        return nullptr;
    }
    const auto byLot = byCategory->second.find(lotId);
    return byLot == byCategory->second.end() ? nullptr : &byLot->second;
}

AuctionService::LotAuction *AuctionService::find(std::size_t category, int lotId)
{
    return const_cast<LotAuction *>(static_cast<const AuctionService *>(this)->find(category, lotId));
}

const AuctionService::Bid *AuctionService::highestBid(std::size_t category, int lotId) const
{
    const LotAuction *auction = find(category, lotId);
    if (!auction || auction->bids.empty())
    {
        return nullptr;
    }
    const Bid *best = &auction->bids.front();
    for (const Bid &bid : auction->bids)
    {
        if (bid.amount > best->amount)
        {
            best = &bid;
        }
    }
    return best;
}

const AuctionService::Bid *AuctionService::bidOf(std::size_t category, int lotId, const std::string &key) const
{
    const LotAuction *auction = find(category, lotId);
    if (!auction || key.empty())
    {
        return nullptr;
    }
    for (const Bid &bid : auction->bids)
    {
        if (bid.owner == key)
        {
            return &bid;
        }
    }
    return nullptr;
}

std::size_t AuctionService::bidCount(std::size_t category, int lotId) const
{
    const LotAuction *auction = find(category, lotId);
    return auction ? auction->bids.size() : 0;
}

std::int64_t AuctionService::endsAt(std::size_t category, int lotId) const
{
    const LotAuction *auction = find(category, lotId);
    return auction ? auction->endsAt : 0;
}

std::int64_t AuctionService::minimumBid(std::size_t category, int lotId) const
{
    Lot lot;
    if (!lotInfo(category, lotId, lot))
    {
        return 0;
    }
    // Стартовая планка фичи — низ торгов; дальше только выше текущей высшей.
    // «Высшая + 1» верна и когда высшая принадлежит самому игроку: свою ставку
    // тоже поднимают только вверх.
    const std::int64_t floor = std::max<std::int64_t>(lot.minPrice, 1);
    const Bid *best = highestBid(category, lotId);
    const std::int64_t minimum = best ? std::max(floor, best->amount + 1) : floor;
    // Минимум выше потолка означал бы «перебить нельзя вообще»: 0 — торги по лоту
    // закрыты для новых ставок, привод объяснит это игроку, а не покажет
    // недостижимое число.
    return minimum > MAX_BID ? 0 : minimum;
}

std::vector<int> AuctionService::lotsOf(std::size_t category, const std::string &ownerKey) const
{
    std::vector<int> lots;
    if (!validCategory(category) || ownerKey.empty())
    {
        return lots;
    }
    const auto byCategory = m_auctions.find(m_categories[category].def.key);
    if (byCategory == m_auctions.end())
    {
        return lots;
    }
    for (const auto &[lotId, auction] : byCategory->second)
    {
        for (const Bid &bid : auction.bids)
        {
            if (bid.owner == ownerKey)
            {
                lots.push_back(lotId);
                break;
            }
        }
    }
    // Порядок обхода unordered_map не определён — сортируем, иначе список прыгал
    // бы между открытиями окна.
    std::sort(lots.begin(), lots.end());
    return lots;
}

// ------------------------------------------------------------------ операции

bool AuctionService::placeBid(std::size_t category, int lotId, const std::string &ownerKey, std::int64_t amount,
                              std::int64_t nowUnix, std::int64_t &refundPrevious, std::string &outbidOwner)
{
    refundPrevious = 0;
    outbidOwner.clear();
    if (!m_loaded || !validCategory(category) || ownerKey.empty() || amount <= 0)
    {
        return false; // до конца загрузки торгов ставку принимать нельзя
    }
    Lot lot;
    if (!lotInfo(category, lotId, lot))
    {
        return false; // лота нет либо он уже не разыгрывается
    }
    // 0 — «перебить нельзя» (высшая ставка упёрлась в потолок), а НЕ «минимума нет»:
    // без этой ветки сравнение `amount < 0` пропустило бы любую сумму, деньги
    // списались бы, а лидером игрок не стал — ставка легла бы вровень с высшей.
    const std::int64_t minimum = minimumBid(category, lotId);
    if (minimum <= 0 || amount < minimum)
    {
        return false;
    }

    LotAuction &auction = m_auctions[m_categories[category].def.key][lotId];
    const Bid *best = highestBid(category, lotId);
    if (best && best->owner != ownerKey)
    {
        outbidOwner = best->owner; // лидер сменился — прежнего привод уведомит
    }

    const std::string &categoryKey = m_categories[category].def.key;
    for (Bid &bid : auction.bids)
    {
        if (bid.owner == ownerKey)
        {
            refundPrevious = bid.amount; // старую сумму привод вернёт целиком
            bid.amount = amount;
            persistBid(categoryKey, lotId, ownerKey, amount);
            return true;
        }
    }

    auction.bids.push_back(Bid{ownerKey, amount});
    if (auction.endsAt == 0)
    {
        auction.endsAt = nowUnix + AUCTION_SECONDS; // первая ставка запускает срок
        persistLot(categoryKey, lotId, auction.endsAt);
    }
    persistBid(categoryKey, lotId, ownerKey, amount);
    return true;
}

std::int64_t AuctionService::cancelBid(std::size_t category, int lotId, const std::string &ownerKey)
{
    LotAuction *auction = find(category, lotId);
    if (!m_loaded || !auction || ownerKey.empty())
    {
        return 0;
    }
    for (auto bid = auction->bids.begin(); bid != auction->bids.end(); ++bid)
    {
        if (bid->owner != ownerKey)
        {
            continue;
        }
        const std::int64_t amount = bid->amount;
        auction->bids.erase(bid);
        const std::string &categoryKey = m_categories[category].def.key;
        if (auction->bids.empty())
        {
            // Никто не торгуется — срок гаснет: он обязан отсчитываться от реальной
            // первой ставки, а не от давно отменённой. Строка лота уходит вместе с
            // последней ставкой (eraseLot снимает и ставки — их уже нет).
            auction->endsAt = 0;
            m_auctions[categoryKey].erase(lotId);
            eraseLot(categoryKey, lotId);
        }
        else
        {
            eraseBid(categoryKey, lotId, ownerKey);
        }
        return amount;
    }
    return 0;
}

void AuctionService::closeLot(std::size_t category, int lotId, const std::string &reason)
{
    LotAuction *auction = find(category, lotId);
    if (!auction)
    {
        return;
    }
    // Копия ДО стирания: обработчик возврата не должен ходить по уже снятому лоту.
    const std::vector<Bid> bids = std::move(auction->bids);
    const std::string &categoryKey = m_categories[category].def.key;
    m_auctions[categoryKey].erase(lotId);
    eraseLot(categoryKey, lotId);
    if (bids.empty() || !m_refund)
    {
        return;
    }
    for (const Bid &bid : bids)
    {
        m_refund(bid, reason);
    }
}

std::vector<AuctionService::Result> AuctionService::resolveDue(std::int64_t nowUnix)
{
    std::vector<Result> results;
    if (!m_loaded)
    {
        return results; // торги ещё читаются из БД — раздача необратима, ждём
    }

    for (std::size_t category = 0; category < m_categories.size(); ++category)
    {
        const CategoryDef &def = m_categories[category].def;
        if (!def.ready())
        {
            continue; // фича ещё не готова раздавать лоты (напр. владение не пришло из БД)
        }
        const auto byCategory = m_auctions.find(def.key);
        if (byCategory == m_auctions.end())
        {
            continue;
        }

        // Ключи, уже забравшие лот в ЭТОМ прогоне: два дозревших лота в один тик не
        // должны обойти правило «один в руки» (eligible увидит выдачу только после
        // award, а он зовётся по ходу цикла).
        std::unordered_set<std::string> takenHere;
        std::vector<int> due;
        for (const auto &[lotId, auction] : byCategory->second)
        {
            if (auction.endsAt != 0 && nowUnix >= auction.endsAt)
            {
                due.push_back(lotId);
            }
        }
        std::sort(due.begin(), due.end()); // порядок разбора не должен зависеть от хеша

        for (const int lotId : due)
        {
            Result result;
            result.category = category;
            result.lotId = lotId;

            std::vector<Bid> ranked = byCategory->second[lotId].bids;
            std::sort(ranked.begin(), ranked.end(),
                      [](const Bid &left, const Bid &right)
                      {
                          return left.amount > right.amount;
                      });

            Lot lot;
            const bool alive = lotInfo(category, lotId, lot);
            for (const Bid &bid : ranked)
            {
                // Лот мог исчезнуть или достаться иначе — тогда победителя нет и
                // деньги возвращаются всем. Иначе берём высшую ставку игрока,
                // которому лот ещё можно отдать.
                if (alive && result.winner.empty() && takenHere.count(bid.owner) == 0 && def.eligible(bid.owner))
                {
                    result.winner = bid.owner;
                    result.winningAmount = bid.amount;
                    takenHere.insert(bid.owner);
                    continue; // победитель уже заплатил ставкой — возврата нет
                }
                result.refunds.push_back(bid);
            }

            byCategory->second.erase(lotId);
            eraseLot(def.key, lotId);
            if (!result.winner.empty())
            {
                def.award(lotId, result.winner); // фича делает свой setOwner и персист
            }
            results.push_back(std::move(result));
        }
    }
    return results;
}

// ------------------------------------------------------------------ возвраты и пометки

void AuctionService::addPendingRefund(const std::string &ownerKey, std::int64_t amount)
{
    if (ownerKey.empty() || amount <= 0)
    {
        return;
    }
    m_pendingRefunds[ownerKey] += amount;
    persistRefundDelta(ownerKey, amount);
}

std::int64_t AuctionService::pendingRefund(const std::string &ownerKey) const
{
    const auto it = m_pendingRefunds.find(ownerKey);
    return it == m_pendingRefunds.end() ? 0 : it->second;
}

std::int64_t AuctionService::takePendingRefund(const std::string &ownerKey)
{
    const auto it = m_pendingRefunds.find(ownerKey);
    if (!m_loaded || it == m_pendingRefunds.end())
    {
        return 0;
    }
    const std::int64_t amount = it->second;
    m_pendingRefunds.erase(it); // снимаем СРАЗУ — повторный вход не выдаст второй раз
    persistRefundDelta(ownerKey, -amount);
    return amount;
}

void AuctionService::markOutbid(const std::string &ownerKey)
{
    if (ownerKey.empty() || !m_outbid.insert(ownerKey).second)
    {
        return; // пометка уже стоит — уведомление однократное
    }
    persistOutbid(ownerKey, true);
}

bool AuctionService::takeOutbid(const std::string &ownerKey)
{
    if (!m_loaded || m_outbid.erase(ownerKey) == 0)
    {
        return false;
    }
    persistOutbid(ownerKey, false);
    return true;
}

// ------------------------------------------------------------------ персист (БД)

std::int64_t AuctionService::accountOf(const std::string &ownerKey)
{
    return ownerKey.empty() ? 0 : std::strtoll(ownerKey.c_str(), nullptr, 10);
}

std::string AuctionService::orderingKeyOfLot(const std::string &categoryKey, int lotId)
{
    // Единица упорядочивания — ЛОТ ЦЕЛИКОМ, а не отдельная ставка: eraseLot сносит
    // строку лота вместе со ВСЕМИ его ставками, поэтому он обязан быть упорядочен и
    // относительно чужих persistBid по этому лоту. Ключ на ставку такую пару не
    // связал бы, и снятие лота могло бы лечь после ставки, поданной уже на новые
    // торги — деньги за неё списаны, а строки в БД нет.
    return fmt::format("auction:{}:{}", categoryKey, lotId);
}

void AuctionService::load()
{
    // Три выборки в ОДНОЙ задаче воркера: торгов единицы, а лишние round-trip
    // растянули бы окно, в котором ставки ещё не принимаются.
    struct Loaded
    {
        std::vector<std::tuple<std::string, int, std::int64_t>> lots;               // категория, лот, срок
        std::vector<std::tuple<std::string, int, std::int64_t, std::int64_t>> bids; // категория, лот, аккаунт, сумма
        std::vector<std::tuple<std::int64_t, std::int64_t, int>> players;           // аккаунт, возврат, пометка
    };

    DatabaseManager::selectQuery<Loaded>(
        [](mysqlx::Schema schema)
        {
            Loaded data;
            mysqlx::RowResult lots = schema.getTable("auction_lot").select("category", "lot_id", "ends_at").execute();
            while (mysqlx::Row row = lots.fetchOne())
            {
                // Каждая строка под своим try: порченое поле теряет ТОЛЬКО её, а не
                // всю выборку — иначе одна битая запись сожгла бы все ставки разом.
                try
                {
                    data.lots.emplace_back(row.get(0).get<std::string>(), row.get(1).get<int>(),
                                           row.get(2).get<std::int64_t>());
                }
                catch (...)
                {
                    continue;
                }
            }
            mysqlx::RowResult bids =
                schema.getTable("auction_bid").select("category", "lot_id", "account_id", "amount").execute();
            while (mysqlx::Row row = bids.fetchOne())
            {
                try
                {
                    data.bids.emplace_back(row.get(0).get<std::string>(), row.get(1).get<int>(),
                                           row.get(2).get<std::int64_t>(), row.get(3).get<std::int64_t>());
                }
                catch (...)
                {
                    continue;
                }
            }
            mysqlx::RowResult players =
                schema.getTable("auction_player").select("account_id", "refund", "outbid").execute();
            while (mysqlx::Row row = players.fetchOne())
            {
                try
                {
                    data.players.emplace_back(row.get(0).get<std::int64_t>(), row.get(1).get<std::int64_t>(),
                                              row.get(2).get<int>());
                }
                catch (...)
                {
                    continue;
                }
            }
            return data;
        },
        [this](Loaded data)
        {
            const std::size_t lotCount = data.lots.size();
            const std::size_t bidCount = data.bids.size();
            for (const auto &[categoryKey, lotId, endsAtUnix] : data.lots)
            {
                if (!categoryKey.empty())
                {
                    m_auctions[categoryKey][lotId].endsAt = endsAtUnix;
                }
            }
            for (const auto &[categoryKey, lotId, accountId, amount] : data.bids)
            {
                if (categoryKey.empty() || accountId <= 0 || amount <= 0)
                {
                    continue;
                }
                m_auctions[categoryKey][lotId].bids.push_back(Bid{std::to_string(accountId), amount});
            }
            // Лот без ставок торгов не ведёт: осиротевшая строка срока (ставки сняли,
            // а строка лота осталась) не должна дозреть в пустой итог.
            for (auto &[categoryKey, byLot] : m_auctions)
            {
                for (auto it = byLot.begin(); it != byLot.end();)
                {
                    it = it->second.bids.empty() ? byLot.erase(it) : std::next(it);
                }
            }
            for (const auto &[accountId, refund, outbid] : data.players)
            {
                if (accountId <= 0)
                {
                    continue;
                }
                const std::string ownerKey = std::to_string(accountId);
                if (refund > 0)
                {
                    m_pendingRefunds[ownerKey] = refund;
                }
                if (outbid != 0)
                {
                    m_outbid.insert(ownerKey);
                }
            }
            m_loaded = true;
            LogManager::log(Message,
                            fmt::format("AuctionService: торги загружены ({} лотов, {} ставок)", lotCount, bidCount));
        },
        [](const std::string &error)
        {
            // Флаг НЕ выставляем: с пустой памятью ставка перезаписала бы чужие
            // строки, а итоги раздали бы лоты мимо реальных ставок. Торги просто
            // ждут перезапуска — деньги в БД целы.
            LogManager::log(Error, "AuctionService: не удалось загрузить торги: " + error);
        });
}

void AuctionService::persistBid(const std::string &categoryKey, int lotId, const std::string &ownerKey,
                                std::int64_t amount) const
{
    const std::int64_t accountId = accountOf(ownerKey);
    if (accountId <= 0)
    {
        return; // не наш формат ключа — писать нечего
    }
    DatabaseManager::throwQueryOrdered(
        orderingKeyOfLot(categoryKey, lotId),
        [categoryKey, lotId, accountId, amount](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO auction_bid (category, lot_id, account_id, amount) VALUES (?, ?, ?, ?) "
                     "ON DUPLICATE KEY UPDATE amount = VALUES(amount)")
                .bind(categoryKey, lotId, accountId, amount)
                .execute();
        },
        [categoryKey, lotId, accountId](const std::string &error)
        {
            LogManager::log(Error, fmt::format("AuctionService: не записана ставка {}/{} аккаунта {}: {}", categoryKey,
                                               lotId, accountId, error));
        });
}

void AuctionService::eraseBid(const std::string &categoryKey, int lotId, const std::string &ownerKey) const
{
    const std::int64_t accountId = accountOf(ownerKey);
    if (accountId <= 0)
    {
        return;
    }
    DatabaseManager::throwQueryOrdered(
        orderingKeyOfLot(categoryKey, lotId),
        [categoryKey, lotId, accountId](mysqlx::Schema schema)
        {
            schema.getTable("auction_bid")
                .remove()
                .where("category = :category AND lot_id = :lot AND account_id = :account")
                .bind("category", categoryKey)
                .bind("lot", lotId)
                .bind("account", accountId)
                .execute();
        },
        [categoryKey, lotId, accountId](const std::string &error)
        {
            LogManager::log(Error, fmt::format("AuctionService: не снята ставка {}/{} аккаунта {}: {}", categoryKey,
                                               lotId, accountId, error));
        });
}

void AuctionService::persistLot(const std::string &categoryKey, int lotId, std::int64_t endsAtUnix) const
{
    DatabaseManager::throwQueryOrdered(
        orderingKeyOfLot(categoryKey, lotId),
        [categoryKey, lotId, endsAtUnix](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO auction_lot (category, lot_id, ends_at) VALUES (?, ?, ?) "
                     "ON DUPLICATE KEY UPDATE ends_at = VALUES(ends_at)")
                .bind(categoryKey, lotId, endsAtUnix)
                .execute();
        },
        [categoryKey, lotId](const std::string &error)
        {
            LogManager::log(Error,
                            fmt::format("AuctionService: не записан срок лота {}/{}: {}", categoryKey, lotId, error));
        });
}

void AuctionService::eraseLot(const std::string &categoryKey, int lotId) const
{
    DatabaseManager::throwQueryOrdered(
        orderingKeyOfLot(categoryKey, lotId),
        [categoryKey, lotId](mysqlx::Schema schema)
        {
            // Срок и ставки — одной транзакцией: лот без срока не дозреет никогда,
            // а срок без ставок даст пустой итог.
            mysqlx::Session &session = schema.getSession();
            session.startTransaction();
            try
            {
                schema.getTable("auction_bid")
                    .remove()
                    .where("category = :category AND lot_id = :lot")
                    .bind("category", categoryKey)
                    .bind("lot", lotId)
                    .execute();
                schema.getTable("auction_lot")
                    .remove()
                    .where("category = :category AND lot_id = :lot")
                    .bind("category", categoryKey)
                    .bind("lot", lotId)
                    .execute();
                session.commit();
            }
            catch (...)
            {
                session.rollback();
                throw;
            }
        },
        [categoryKey, lotId](const std::string &error)
        {
            LogManager::log(Error, fmt::format("AuctionService: не снят лот {}/{}: {}", categoryKey, lotId, error));
        });
}

void AuctionService::persistRefundDelta(const std::string &ownerKey, std::int64_t delta) const
{
    const std::int64_t accountId = accountOf(ownerKey);
    if (accountId <= 0 || delta == 0)
    {
        return;
    }
    // ОТНОСИТЕЛЬНАЯ запись, а не абсолютная. Задачи уходят в многопоточный пул и
    // порядок их выполнения не определён: два возврата, начисленные в один тик
    // итогов, при абсолютной записи затёрли бы друг друга и съели деньги.
    //
    // Обе ветки применяют ОДНУ И ТУ ЖЕ дельту и ничем её не клампят: безопасной
    // перестановку задач делает именно коммутативность сложения. Кламп нижней
    // границей (GREATEST(refund + ?, 0)) её ломает — «-N», выполнившийся раньше
    // своего «+N», обрезался бы в ноль, а пришедший следом «+N» вернул бы в БД уже
    // выданную наличными сумму, и на ближайшем рестарте она выдалась бы второй раз.
    // То же и у ветки INSERT: клампить стартовое значение нельзя по той же причине.
    //
    // Отрицательный остаток в строке поэтому допустим: он промежуточный и гасится
    // парным начислением, а до игрока не доходит — загрузка берёт строку только при
    // refund > 0 (см. load).
    DatabaseManager::throwQuery(
        [accountId, delta](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO auction_player (account_id, refund, outbid) VALUES (?, ?, 0) "
                     "ON DUPLICATE KEY UPDATE refund = refund + ?")
                .bind(accountId, delta, delta)
                .execute();
        },
        [accountId, delta](const std::string &error)
        {
            LogManager::log(Error, fmt::format("AuctionService: не записан возврат {} аккаунту {}: {}", delta,
                                               accountId, error));
        });
}

void AuctionService::persistOutbid(const std::string &ownerKey, bool marked) const
{
    const std::int64_t accountId = accountOf(ownerKey);
    if (accountId <= 0)
    {
        return;
    }
    // Пометка — отдельная колонка отдельным запросом: смешивать её с суммой в одной
    // абсолютной записи нельзя (см. persistRefundDelta). Строку с нулями НЕ удаляем:
    // DELETE, переставший местами с INSERT возврата, стёр бы деньги. Пустая строка
    // на аккаунт безвредна, загрузка её просто пропускает.
    const int flag = marked ? 1 : 0;
    DatabaseManager::throwQuery(
        [accountId, flag](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO auction_player (account_id, refund, outbid) VALUES (?, 0, ?) "
                     "ON DUPLICATE KEY UPDATE outbid = VALUES(outbid)")
                .bind(accountId, flag)
                .execute();
        },
        [accountId](const std::string &error)
        {
            LogManager::log(Error, fmt::format("AuctionService: не записана пометка аккаунта {}: {}", accountId, error));
        });
}
