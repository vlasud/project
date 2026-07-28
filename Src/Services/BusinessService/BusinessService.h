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
// ХРАНЕНИЕ: businesses.json рядом с сервером (как houses.json) — описание И
// владение. Дома владение держат в БД, потому что там оно менялось задолго до
// json-описания; у бизнесов покупка ещё не введена, поэтому отдельная таблица не
// заводится: когда появится покупка за деньги, владение переедет в БД тем же
// путём, что house_owner.
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
        std::string owner;      // ключ аккаунта (std::to_string(accountId)); "" — ничейный
        std::int64_t price = 0; // цена покупки игроком (дев задаёт при создании)
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

    // --- операции (источник правды; персист делает BusinessSystem) ---
    // Создать бизнес в позиции создателя. Точка выхода — за спиной по его углу.
    // nullptr — тип не зарегистрирован, индекс каталога невалиден или упёрлись в
    // MAX_BUSINESS_ID.
    const Business *createBusiness(Type type, const Vector3 &creatorPos, float creatorAngle, int interiorIndex,
                                   std::int64_t price);
    bool removeBusiness(int id);
    // Сменить владельца в памяти. "" — снять владение. Персист (json) — на приводе,
    // он подписан на subscribeChanged.
    bool setOwner(int id, const std::string &ownerKey);
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
    TypeDef m_types[static_cast<std::size_t>(Type::Count)];
    std::vector<ChangedObserver> m_changedObservers;
};
