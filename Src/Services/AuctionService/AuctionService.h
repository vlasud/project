#pragma once

#include "Services/IService.h"
#include "types.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class AuctionSystem;
struct IPlayer;

// Аукционы — ОДИН механизм торгов на все фичи (бизнес-сервис, НЕ Core). Здесь
// живут САМИ ТОРГИ: ставки, сроки, невыданные возвраты, пометки «тебя перебили».
// Что именно разыгрывается — сервису безразлично.
//
// САМОРЕГИСТРАЦИЯ КАТЕГОРИЙ (как работы в JobDismissService, типы бизнеса в
// BusinessService): фича приносит ключ, имя и четыре колбэка — «расскажи о лоте»,
// «может ли этот аккаунт получить лот», «выдай лот победителю», «готова ли ты
// подводить итоги». Ни бизнесов, ни домов сервис не знает. Новая аукционная
// фича — одна регистрация; ни торги, ни окна не правятся.
//
// ПОЧЕМУ ТОРГИ ЗДЕСЬ, А НЕ В КАЖДОЙ ФИЧЕ: правила у них одинаковые до буквы
// (деньги списываются на ставке, перебить можно только вверх, срок идёт с первой
// ставки, проигравшим — возврат). Две копии этого кода разъехались бы на первой
// же правке, а расхождение здесь стоит игрокам денег.
//
// ДЕНЬГИ СПИСЫВАЮТСЯ В МОМЕНТ СТАВКИ, поэтому торги ОБЯЗАНЫ переживать рестарт —
// и живут В БД (auction_lot / auction_bid / auction_player), write-through прямо
// отсюда, как членство в FamilyService. json-файлы фич — дев-КОНТЕНТ (описание
// точек), в проде они read-only; вся ДИНАМИКА идёт в БД.
//
// Срок — абсолютное unix-время, а не таймер в памяти: отсчёт должен идти и пока
// сервер лежит. Ключ лота в БД — СТРОКОВЫЙ ключ категории, а не её индекс: порядок
// регистрации меняется правкой SystemRegister, и индексы сдвинули бы чужие ставки.
// Строки незнакомых категорий (фичу временно убрали) лежат нетронутыми — их просто
// некому показать, но деньги за ними целы.
//
// ВСЁ ДЕРЖИТСЯ В ПАМЯТИ и грузится ЦЕЛИКОМ на старте (торгов единицы): БД тут —
// только персист, а не оперативное хранилище. До конца загрузки ставки не
// принимаются и итоги не подводятся: писать поверх непрочитанного — терять деньги.
class AuctionService final : public IService
{
    friend AuctionSystem;

  public:
    // Сколько длятся торги с ПЕРВОЙ ставки.
    static constexpr std::int64_t AUCTION_SECONDS = 2 * 24 * 60 * 60;

    // Ставка. Одна на аккаунт в пределах лота.
    struct Bid
    {
        std::string owner; // ключ аккаунта (std::to_string(accountId))
        std::int64_t amount = 0;
    };

    // Что фича рассказывает о лоте окну торгов.
    struct Lot
    {
        std::string title;       // utf-8: «Дом #4», «Бизнес #3 — Магазин 24/7»
        std::string description; // utf-8: строки подробностей (могут быть пустыми)
        Vector3 position{};      // точка в мире — цель «отметить на GPS»
        std::int64_t minPrice = 0; // стартовая планка торгов
    };

    // Итог разыгранного лота. Владение выставляет ФИЧА (колбэк award) — сервис не
    // знает, что значит «стать владельцем», и не лезет ни в чей персист.
    struct Result
    {
        std::size_t category = 0;
        int lotId = 0;
        std::string winner; // "" — победителя нет (ставок не было либо все не подходят)
        std::int64_t winningAmount = 0;
        std::vector<Bid> refunds; // проигравшим и отсеянным — деньги назад
    };

    // Рассказать о лоте. false — лота больше нет либо он уже не разыгрывается
    // (появился владелец): торги по нему закрываются, ставки возвращаются.
    using LotInfo = std::function<bool(int lotId, Lot &out)>;
    // Может ли аккаунт получить лот этой категории (напр. «один дом в руки»).
    using Eligible = std::function<bool(const std::string &ownerKey)>;
    // Выдать лот победителю (фича делает свой setOwner и свой персист).
    using Award = std::function<void(int lotId, const std::string &ownerKey)>;
    // Готова ли категория подводить итоги (напр. владение подтянулось из БД).
    // Пока false — лоты категории не разыгрываются: раздача необратима.
    using Ready = std::function<bool()>;

    struct CategoryDef
    {
        std::string key;  // СТАБИЛЬНЫЙ ключ для файла: "business", "house"
        std::string name; // utf-8 для окна: «Бизнесы», «Дома»
        LotInfo info;
        Eligible eligible;
        Award award;
        Ready ready;
    };

    // Открыть игроку окно торгов по лоту. Ставит AuctionSystem (bindWindow) —
    // окна общие для всех категорий, как и сами торги.
    using WindowHandler = std::function<void(IPlayer &player, std::size_t category, int lotId)>;
    // Вернуть деньги за ставку. Тоже ставит AuctionSystem (bindRefund): сервис знает
    // суммы, но выдавать наличные (или откладывать выдачу офлайновому) — дело
    // привода, и путь денег обязан быть один на все аукционы.
    using RefundHandler = std::function<void(const Bid &bid, const std::string &reason)>;

    // --- реестр категорий (из конструкторов систем-владельцев лотов) ---
    void registerCategory(CategoryDef def);
    std::size_t categoryCount() const;
    const std::string &categoryName(std::size_t category) const;
    // Индекс категории по её стабильному ключу; -1 — не зарегистрирована.
    int categoryByKey(const std::string &key) const;
    // Данные лота от фичи. false — лот исчез либо больше не разыгрывается.
    bool lotInfo(std::size_t category, int lotId, Lot &out) const;
    bool eligible(std::size_t category, const std::string &ownerKey) const;

    void bindWindow(WindowHandler handler);
    void bindRefund(RefundHandler handler);
    // Открыть окно торгов (no-op без bindWindow либо при неизвестной категории).
    void openLot(IPlayer &player, std::size_t category, int lotId) const;

    // --- запрос торгов ---
    const Bid *highestBid(std::size_t category, int lotId) const;               // nullptr — ставок нет
    const Bid *bidOf(std::size_t category, int lotId, const std::string &key) const;
    std::size_t bidCount(std::size_t category, int lotId) const;
    std::int64_t endsAt(std::size_t category, int lotId) const;                 // 0 — срок не идёт
    // Минимально принимаемая ставка: не ниже стартовой планки лота и строго выше
    // текущей высшей. 0 — лота нет.
    std::int64_t minimumBid(std::size_t category, int lotId) const;
    // Лоты категории, где у аккаунта есть ставка (отсортированы по id).
    std::vector<int> lotsOf(std::size_t category, const std::string &ownerKey) const;

    // --- операции ---
    // Поставить ставку. refundPrevious — прежняя ставка ЭТОГО игрока (деньги за неё
    // уже списаны, вернуть обязан привод). outbidOwner — тот, чью лидирующую ставку
    // эта перебила ("" — никого). false — категории/лота нет, лот уже не
    // разыгрывается, ставка не выше минимума либо ключ пуст.
    bool placeBid(std::size_t category, int lotId, const std::string &ownerKey, std::int64_t amount,
                  std::int64_t nowUnix, std::int64_t &refundPrevious, std::string &outbidOwner);
    // Снять свою ставку: сумма к возврату (0 — ставки не было). Ставок не осталось —
    // срок гаснет, следующая первая ставка запустит его заново.
    std::int64_t cancelBid(std::size_t category, int lotId, const std::string &ownerKey);
    // Снять лот с торгов (лот снесли/достался иначе): все ставки уходят на возврат
    // через RefundHandler. Зовёт фича, когда её лот перестал существовать — иначе
    // списанные деньги игроков просто сгорели бы вместе с лотом.
    void closeLot(std::size_t category, int lotId, const std::string &reason);
    // Подвести итоги дозревших лотов ВСЕХ готовых категорий. Победителю лот выдан
    // здесь же через award; деньги проигравшим раздаёт привод по возвращённому списку.
    std::vector<Result> resolveDue(std::int64_t nowUnix);

    // --- невыданные возвраты (проигравший был офлайн) ---
    void addPendingRefund(const std::string &ownerKey, std::int64_t amount);
    std::int64_t pendingRefund(const std::string &ownerKey) const;
    std::int64_t takePendingRefund(const std::string &ownerKey);

    // --- пометка «твою ставку перебили» (перебили, пока он был офлайн) ---
    void markOutbid(const std::string &ownerKey);
    bool takeOutbid(const std::string &ownerKey); // true — пометка была, снята

    // Загружены ли торги из БД. До этого ставка не принимается, а итоги не
    // подводятся (см. комментарий к классу).
    bool isLoaded() const
    {
        return m_loaded;
    }

  private:
    // --- вызывается AuctionSystem ---
    // Прочитать ВСЕ торги из БД (один запрос на старте). По завершении — isLoaded.
    void load();

    // --- write-through (внутри сервиса, как в FamilyService) ---
    static std::int64_t accountOf(const std::string &ownerKey); // 0 — ключ не число
    void persistBid(const std::string &categoryKey, int lotId, const std::string &ownerKey, std::int64_t amount) const;
    void eraseBid(const std::string &categoryKey, int lotId, const std::string &ownerKey) const;
    void persistLot(const std::string &categoryKey, int lotId, std::int64_t endsAtUnix) const;
    // Снять лот целиком: его строка и ВСЕ его ставки одной транзакцией.
    void eraseLot(const std::string &categoryKey, int lotId) const;
    // Возврат и пометка одного аккаунта; нули — строка удаляется.
    void persistPlayer(const std::string &ownerKey) const;

    struct LotAuction
    {
        std::int64_t endsAt = 0; // абсолютное unix-время; 0 — торги не начаты
        std::vector<Bid> bids;
    };

    struct Category
    {
        CategoryDef def;
    };

    const LotAuction *find(std::size_t category, int lotId) const;
    LotAuction *find(std::size_t category, int lotId);
    bool validCategory(std::size_t category) const;

    std::vector<Category> m_categories;
    // categoryKey -> lotId -> торги. Ключ строковый: см. комментарий к классу.
    std::unordered_map<std::string, std::unordered_map<int, LotAuction>> m_auctions;
    std::unordered_map<std::string, std::int64_t> m_pendingRefunds;
    std::unordered_set<std::string> m_outbid;
    bool m_loaded = false; // торги прочитаны из БД
    WindowHandler m_window;
    RefundHandler m_refund;
};
