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
        Shop247, // продуктовый магазин 24/7
        Count
    };

    // Готовый интерьер типа: имя в дев-меню + interior id SA + точка спавна внутри.
    struct CatalogEntry
    {
        std::string name; // utf-8, отображается в LIST
        int interiorId;
        Vector3 insideSpawn;
        float insideAngle;
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
        std::int64_t price = 0;   // стартовая планка аукциона (дев задаёт при создании)
        std::int64_t balance = 0; // накопленный доход, ждёт снятия владельцем
    };

    // --- реестр типов (из конструкторов систем-владельцев типов) ---
    void registerType(Type type, std::string name, std::vector<CatalogEntry> catalog, VisitorMenu visitorMenu);
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
    bool addIncome(int id, std::int64_t income);
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
        std::string name;
        std::vector<CatalogEntry> catalog;
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
    std::vector<OwnerChangedObserver> m_ownerChangedObservers;
};
