#pragma once

#include "Services/IService.h"
#include "types.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class BusinessSystem;
struct IPlayer;

// Бизнесы — принадлежащие игроку доходные точки (бизнес-фича, НЕ Core). Бизнес это
// пикап ВХОДА в позиции создателя + ИНТЕРЬЕР из каталога своего ТИПА; внутри —
// пикап выхода. id — порядковый номер создания.
//
// ТИПЫ РЕГИСТРИРУЮТСЯ САМИ (как работы в JobDismissService): система конкретного
// бизнеса приносит своё имя, свой каталог интерьеров и обработчик «открыть меню
// посетителю». Сервис про геймплей типов не знает вовсе — он держит только то, что
// у всех бизнесов общее: кому принадлежит, где стоит и сколько накопил. Новый тип
// бизнеса — своя система + одна регистрация, сервис и общее меню не правятся.
//
// НИЧЕЙНЫЙ БИЗНЕС РАЗЫГРЫВАЕТСЯ АУКЦИОНОМ, и торги здесь НЕ живут: ставки, сроки
// и возвраты ведёт общий AuctionService (см. Docs/Auction.md) — правила у всех
// аукционов одинаковые, и второй копии этого кода в проекте нет. Отсюда торгам
// отдают только категорию «Бизнесы»: как выглядит лот, кому его можно отдать
// (ОДИН БИЗНЕС НА АККАУНТ, ownsBusiness) и как передать победителю (setOwner).
//
// ДВА РАЗНЫХ ИСТОЧНИКА ПРАВДЫ (как у домов, см. HouseService):
//  * ОПИСАНИЕ бизнеса — businesses.json;
//  * ВЛАДЕНИЕ (бизнес -> аккаунт) — БД (таблица business_owner, write-through).
//    Business::owner — лишь in-memory зеркало: наполняется из БД на старте
//    (BusinessSystem грузит business_owner ПОСЛЕ спавна точек) и на смене
//    владельца. Запись в БД делает BusinessSystem по subscribeOwnerChanged.
//
// vw бизнеса — уникальный (VW_BASE + id), чтобы интерьеры разных точек не
// пересекались; снаружи (вход/иконка) — основной мир (vw 0).
class BusinessService final : public IService
{
    friend BusinessSystem;

  public:
    // База уникального виртуального мира. Отличается от домов (2000000), иначе
    // интерьер бизнеса и интерьер дома делили бы один мир.
    static constexpr int VW_BASE = 3000000;
    static constexpr int MAX_BUSINESS_ID = 1000000;

    enum class Type
    {
        Shop247,    // продуктовый магазин 24/7
        GasStation, // АЗС: та же витрина + заправка машин снаружи
        Count
    };

    // Готовый интерьер типа: имя в дев-меню + interior id SA + точка спавна внутри.
    struct CatalogEntry
    {
        std::string name; // utf-8, отображается в LIST
        int interiorId;
        Vector3 insideSpawn; // куда попадает игрок, войдя внутрь
        float insideAngle;   // лицом куда он появляется (yaw, градусы)
        // ПРИЛАВОК — точка, встав на которую посетитель получает витрину. Замер, как
        // и вход: у каждого интерьера касса своя. НЕ ЗАДАН (нулевой вектор —
        // интерьерных координат (0,0,0) не бывает) — чекпоинта у точки нет, витрина
        // открывается только через меню бизнеса.
        //
        // Точки пикапа ВЫХОДА здесь нет намеренно: выходы из интерьеров считаются,
        // а не замеряются (см. Docs/Business.md). Статичны вход, угол и прилавок.
        Vector3 counter{};
    };

    // Товар на полке ТИПА: что продаём, почём и сколько влезает на склад точки.
    // Живёт в реестре типа, а не в витрине: список нужен двоим — витрине (продажа) и
    // меню владельца (инвентаризация, заказ), и вторая копия разъехалась бы.
    struct GoodDef
    {
        int itemType;
        std::int64_t price;
        int stockCap;            // максимум этого товара на складе одной точки
        std::string description; // справка для витрины (utf-8)
        std::string popupName;   // короткая английская метка для попапа
    };

    // УСЛУГА точки: продаётся механика, а не предмет. Склада у услуги нет
    // (телефонный номер не кончается), поэтому она НЕ GoodDef. Деньги и доход
    // проводит сам обработчик: продажа бывает отложенной (номер проверяется в БД) и
    // может вообще не состояться — витрина об этом ничего знать не должна.
    struct ServiceDef
    {
        std::string name;        // «Телефон», utf-8
        std::int64_t price = 0;  // отображается в витрине; списывает обработчик
        std::string description; // справка для витрины (utf-8)
        std::string popupName;   // короткая английская метка попапа
        // Что писать в колонке «У вас» (utf-8). Не задан — прочерк.
        std::function<std::string(int playerId)> status;
        // Клик по «Купить». Витрина деньги НЕ списывает и склад не трогает.
        std::function<void(IPlayer &player, int businessId)> sell;
    };

    // Меню бизнеса для ПОСЕТИТЕЛЯ: геймплей типа (у 24/7 — покупка товаров).
    // Зовётся из общего интерфейса «Бизнес», когда игрок внутри.
    using VisitorMenu = std::function<void(IPlayer &player, int businessId)>;

    struct Business
    {
        int id = 0;
        Type type = Type::Shop247;
        int interiorIndex = 0;
        Vector3 entrance{};
        Vector3 exit{};
        float exitAngle = 0.0f;
        int virtualWorld = 0;
        std::string owner;      // ключ аккаунта (std::to_string(accountId)); "" — ничейный.
                                // Зеркало БД (business_owner), а НЕ json.
        std::int64_t price = 0;   // госцена точки (дев задаёт при создании)
        std::int64_t balance = 0; // накопленный доход, ждёт снятия владельцем
        // СКЛАД точки: тип предмета -> сколько лежит. Продажа списывает отсюда,
        // заказ пополняет. Зеркало БД (business_stock), в json НЕ пишется — это
        // динамика, а json дев-контент.
        std::unordered_map<int, int> stock;
    };

    // --- реестр типов (из конструкторов систем-владельцев типов) ---
    // popupName — КОРОТКАЯ метка для экранного попапа («24/7», «Gas Station»).
    // Отдельно от name: попапы проекта всегда на английском, а name русское и живёт
    // в диалогах и дев-меню.
    void registerType(Type type, std::string name, std::string popupName, std::vector<CatalogEntry> catalog,
                      std::vector<GoodDef> goods, VisitorMenu visitorMenu);
    // Услугу добавляет система-владелец МЕХАНИКИ (телефон регистрирует себя сам),
    // а не система типа бизнеса. Порядок относительно registerType не важен: список
    // услуг живёт отдельно от регистрации типа и ею не затирается — порядок
    // конструирования систем ничего не гарантирует.
    void addService(Type type, ServiceDef service);
    const std::vector<ServiceDef> &services(Type type) const;
    const std::string &typePopupName(Type type) const;
    const std::vector<GoodDef> &goods(Type type) const;
    bool typeRegistered(Type type) const;
    const std::string &typeName(Type type) const;
    const std::vector<CatalogEntry> &catalog(Type type) const;
    bool catalogValid(Type type, int interiorIndex) const;
    // Открыть посетителю меню геймплея этого бизнеса. false — тип не зарегистрирован.
    bool openVisitorMenu(Type type, IPlayer &player, int businessId) const;
    // Зарегистрированные типы в порядке объявления enum (для дев-меню выбора типа).
    std::vector<Type> registeredTypes() const;

    // --- запрос состояния ---
    const Business *getBusiness(int id) const;
    const std::unordered_map<int, Business> &businesses() const
    {
        return m_businesses;
    }
    std::size_t count() const
    {
        return m_businesses.size();
    }
    // Бизнесы игрока (ключ аккаунта). Пустой ключ — пусто. Линейно, холодный путь.
    std::vector<int> businessesOf(const std::string &ownerKey) const;
    // Владеет ли аккаунт хоть каким-то бизнесом (один бизнес на аккаунт). Пустой
    // ключ — всегда false. Линейно по бизнесам (десятки), холодный путь.
    bool ownsBusiness(const std::string &ownerKey) const;

    // Владение из БД легло в память (одноразовое стартовое событие). До этого
    // зеркало неполно, поэтому итоги аукционов НЕ подводятся: «один бизнес на
    // аккаунт» нечем проверить, а победитель — необратимая раздача.
    bool isOwnershipLoaded() const
    {
        return m_ownershipLoaded;
    }

    // --- операции (источник правды; персист делает BusinessSystem) ---
    // Создать бизнес в позиции создателя. Точка выхода — за спиной по его углу.
    // nullptr — тип не зарегистрирован, индекс каталога невалиден или упёрлись в
    // MAX_BUSINESS_ID.
    const Business *createBusiness(Type type, const Vector3 &creatorPos, float creatorAngle, int interiorIndex,
                                   std::int64_t price);
    bool removeBusiness(int id);
    // Сменить стартовую планку торгов (дев-правка уже созданного бизнеса).
    // Отрицательная клампится к нулю. false — бизнеса нет.
    bool setPrice(int id, std::int64_t price);
    // Сменить владельца в ПАМЯТИ (зеркало БД). "" — снять владение. false — бизнеса
    // нет. Запись в business_owner делает привод по subscribeOwnerChanged — ЕДИНАЯ
    // точка персиста владения для всех путей (итог аукциона, снос, выселение).
    bool setOwner(int id, const std::string &ownerKey);

    // Наблюдатель смены владельца (после успешного setOwner): id, старый и новый
    // ключ ("" — ничейный). Привод пишет БД и откатывает память при сбое.
    using OwnerChangedObserver = std::function<void(int businessId, const std::string &oldKey,
                                                    const std::string &newKey)>;
    void subscribeOwnerChanged(OwnerChangedObserver observer);
    // Начислить доход в копилку бизнеса (income > 0). false — бизнеса нет.
    //
    // Покупатель роли НЕ играет, в том числе если он же и владелец. Раньше выручка с
    // самого владельца отбрасывалась: без склада он выкупал у себя товар, забирал те
    // же деньги из копилки и получал вещь даром. Теперь товар кончается и его надо
    // закупать (ORDER_PRICE_PERCENT от цены продажи), поэтому каждая покупка у себя
    // съедает единицу и стоит владельцу закупочной цены — она убыточна сама по себе,
    // и особый случай в коде больше не нужен.
    bool addIncome(int id, std::int64_t income);

    // Доход ЗАЧИСЛЕН в копилку: (бизнес, сумма). Привод пишет из этого историю
    // выручки по дням в БД — сам сервис в БД не ходит (как со сменой владельца).
    using IncomeObserver = std::function<void(int businessId, std::int64_t income)>;
    void subscribeIncome(IncomeObserver observer);

    // --- склад точки ---
    int stockOf(int businessId, int itemType) const;
    // Сколько ЕЩЁ влезет этого товара (потолок минус остаток). 0 — склад полон либо
    // товар не из ассортимента этого типа.
    int stockRoom(int businessId, int itemType) const;
    // Списать со склада при продаже. false — на складе столько нет (продажа не идёт).
    bool consumeStock(int businessId, int itemType, int count);
    // Пополнить (доставка заказа). Клампится потолком товара; возвращает сколько
    // реально легло.
    int addStock(int businessId, int itemType, int count);
    // Загрузка зеркала из БД — БЕЗ нотификации (иначе write-through обратно в БД).
    void loadStock(int businessId, int itemType, int quantity);

    // Остаток изменился: (бизнес, товар, новый остаток). Привод пишет в БД.
    using StockObserver = std::function<void(int businessId, int itemType, int quantity)>;
    void subscribeStockChanged(StockObserver observer);
    // Забрать всю копилку: возвращает снятую сумму (0 — пусто/нет бизнеса).
    std::int64_t withdrawBalance(int id);

    // Любое изменение, требующее записи файла (создание/удаление/владелец/копилка).
    using ChangedObserver = std::function<void()>;
    void subscribeChanged(ChangedObserver observer);

    // --- сериализация JSON ---
    std::string serialize() const;

  private:
    // --- вызывается BusinessSystem ---
    void loadBusiness(const Business &business);
    void finalizeLoad();
    // Владелец в памяти БЕЗ нотификации: загрузка зеркала из БД (иначе каждая
    // строка спровоцировала бы write-through обратно) и откат при сбое записи
    // (иначе рекурсия зациклила бы попытки при затяжном сбое БД).
    bool setOwnerSilent(int id, const std::string &ownerKey);
    void markOwnershipLoaded();

    struct TypeDef
    {
        bool registered = false;
        std::string name;      // русское, для диалогов и дев-меню
        std::string popupName; // короткое английское, для экранных попапов
        std::vector<CatalogEntry> catalog;
        std::vector<GoodDef> goods;
        // Услуги живут ВНЕ registerType: их кладут чужие системы, и регистрация
        // типа (которая может прийти позже) не имеет права их потерять.
        std::vector<ServiceDef> services;
        VisitorMenu visitorMenu;
    };

    static bool validType(Type type);
    void notifyChanged() const;
    static Vector3 backOf(const Vector3 &position, float angleDegrees, float distance);

    std::unordered_map<int, Business> m_businesses;
    int m_nextId = 1;
    bool m_ownershipLoaded = false; // зеркало business_owner легло в память
    TypeDef m_types[static_cast<std::size_t>(Type::Count)];
    std::vector<ChangedObserver> m_changedObservers;
    std::vector<IncomeObserver> m_incomeObservers;
    std::vector<StockObserver> m_stockObservers;
    // Потолок товара у типа этой точки; -1 — товара нет в ассортименте.
    int stockCapOf(int businessId, int itemType) const;
    std::vector<OwnerChangedObserver> m_ownerChangedObservers;
};
