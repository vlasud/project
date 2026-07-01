#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "player.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class FactionSystem;

// Базовая система фракций — источник правды о фракциях, рангах и членстве
// игроков онлайн. Сами фракции — часть кода (новая фракция = новая система,
// без кода её не добавить), поэтому регистрируются кодом через
// registerFaction(). Ранги и члены — данные: правятся лидером в рантайме и
// связаны с аккаунтами, поэтому живут в БД (Sql/factions.sql): ранги грузятся
// на старте, членство — при старте сессии; изменения уходят в БД сразу
// (write-through).
//
// Гос-вертикаль — через ФРАКЦИЮ-КУРАТОРА: у фракции может быть надзорная
// фракция (supervisorId). Лидер куратора управляет ВСЕМИ подопечными; рангам
// куратора лидер передаёт в управление КОНКРЕТНЫЕ организации — список
// managed у ранга (canManage/appointLeader/dismissLeader). Пример: президент
// (лидер администрации, избирается) создаёт ранг «Министр ВД» и отмечает ему
// подопечными полиции трёх городов — министр управляет только ими.
//
// Расширение конкретной фракцией (ЛСПД, мафия, ...) — своя система:
//  * в конструкторе регистрирует свою фракцию registerFaction(id, имя) —
//    id фиксированный, на него завязаны ранги/члены в БД;
//  * подписывается subscribeMemberChange() и реагирует на вход/выход/появление
//    члена своей фракции в сети (фильтруя по factionId);
//  * регистрирует свои биты маски registerPermission() и проверяет их в
//    командах hasPermission(playerId, маска); лидер имеет все биты;
//  * геймплей (дежурства, склад, краски) — целиком её, базе об этом знать
//    не нужно.
//
// Лидер (флаг члена) управляет рангами своей фракции и людьми. У ранга —
// ТОЛЬКО название и МАСКА ДОСТУПА (никаких числовых лестниц 1..10). Базовые
// биты есть у всех организаций:
//  * PERM_INVITE — приглашать игроков (и выставлять зарплату);
//  * PERM_FIRE   — увольнять (лидера уволить нельзя);
//  * PERM_BUDGET — доступ к бюджету.
// Конкретная организация расширяет маску своими битами (с FIRST_CUSTOM_BIT):
// registerPermission(factionId, маска, имя) — имя видно лидеру в меню ранга,
// проверка в коде — hasPermission(playerId, маска). У лидера все биты всегда.
//
// Зарплата — ПЕРСОНАЛЬНАЯ у члена (прописывается при найме) и НЕ платится
// сама. Деньги организаций лежат в банке (бюджет — её счёт), выплата — по
// ПРИКАЗУ лидера (не чаще PAY_ORDER_COOLDOWN): сумма всех зарплат списывается
// с бюджета, каждому члену (и оффлайн) зачисляется чек на банковский счёт
// (BankService, суммы копятся). Не хватило бюджета на всех — приказ не
// проходит целиком. Оркестрация в FactionSystem: здесь кулдаун и бюджет.
//
// Все входные данные лидера — клиентские, поэтому зажаты: имя ранга
// санитизируется и ограничено по длине, доступ 1..9, зарплата 0..MAX_SALARY
// (выплата — деньги!), число рангов <= MAX_RANKS, последний ранг удалить
// нельзя, бюджет 0..MAX_BUDGET.
//
// Текст в памяти и БД — utf-8 (ввод из диалогов конвертируй cp1251Toutf8 до
// передачи сюда; отображение — через utf8Tocp1251).
class FactionService final : public IService
{
    friend FactionSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    static constexpr int NO_FACTION = 0;
    static constexpr std::int64_t MAX_SALARY = 100000;
    static constexpr std::int64_t MAX_BUDGET = 1000000000;
    static constexpr std::size_t MAX_RANKS = 30;
    static constexpr std::size_t MAX_RANK_NAME_BYTES = 48; // utf-8, ~24 кириллических

    // Маска доступа ранга. Биты 0..7 — базовые (common), у всех организаций;
    // биты конкретных организаций — с FIRST_CUSTOM_BIT.
    using PermissionMask = std::uint64_t;
    static constexpr PermissionMask PERM_INVITE = 1ull << 0; // приглашение + зарплата при найме
    static constexpr PermissionMask PERM_FIRE = 1ull << 1;   // увольнение
    static constexpr PermissionMask PERM_BUDGET = 1ull << 2; // доступ к бюджету
    static constexpr PermissionMask PERM_SKIN = 1ull << 3;   // смена скина из пула организации
    static constexpr PermissionMask COMMON_PERMISSIONS = PERM_INVITE | PERM_FIRE | PERM_BUDGET | PERM_SKIN;
    static constexpr int FIRST_CUSTOM_BIT = 8;

    struct PermissionDef // для лидерского UI (тогглы в меню ранга)
    {
        PermissionMask mask = 0;
        std::string name; // utf-8
    };

    static constexpr Minutes PAY_ORDER_COOLDOWN{30};

    struct Rank
    {
        std::int64_t id = 0; // внутренний ключ строки БД, в геймплее не светится
        std::string name;    // utf-8
        PermissionMask permissions = 0;
        // Подопечные организации, переданные этому рангу в управление лидером
        // куратора (id фракций с supervisorId == наша). Назначение/снятие их
        // лидеров через /gov.
        std::vector<int> managed;
        // Стартовый ранг «Без ранга»: есть у каждой фракции (создаётся при
        // загрузке, если его нет), удалить нельзя. Новички и обладатели
        // удалённых рангов попадают на него.
        bool isDefault = false;
    };

    // «Дверь» базы: пикап и точка, куда он переносит.
    struct BaseDoor
    {
        Vector3 pickupPos{}; // где стоит пикап
        Vector3 targetPos{}; // куда переносит игрока
        float targetAngle = 0.0f;
    };

    // База организации: здание с интерьером, входы на улице и выходы внутри
    // (дверей может быть несколько). Вход — только членам фракции; внутри
    // изоляция по виртуальному миру (= id фракции), поэтому базы разных
    // организаций могут делить один интерьер, не видя друг друга.
    struct Base
    {
        int pickupModel = 1318; // стрелка
        int interior = 0;       // интерьер базы
        std::vector<BaseDoor> entrances; // улица (мир 0) -> интерьер базы
        std::vector<BaseDoor> exits;     // интерьер (мир = id фракции) -> улица
        bool defined = false;            // выставляет registerBase
    };

    // Точка спавна организации: члены появляются здесь (вместо гражданского
    // спавна) при каждом спавне, пока состоят во фракции.
    struct Spawn
    {
        Vector3 position{};
        float angle = 0.0f;
        int interior = 0;
        int virtualWorld = 0; // для точки внутри базы — id фракции
        bool defined = false; // выставляет registerSpawn
    };

    struct Faction
    {
        int id = NO_FACTION;
        std::string name; // utf-8
        // Уникальный цвет организации: ник и маркер на карте у членов.
        Colour colour = Colour::White();
        // Пул скинов организации: члены с PERM_SKIN выбирают из него (/skin).
        std::vector<int> skins;
        std::int64_t budget = 0;
        int supervisorId = NO_FACTION; // фракция-куратор (NO_FACTION — никто)
        std::vector<PermissionDef> customPermissions; // расширение маски этой организации
        std::vector<Rank> ranks;
        Base base;
        Spawn spawn;
        TimePoint orderIssuedAt{}; // кулдаун приказов о выплате (память, сессия сервера)
    };

    // --- справочник ---
    // Регистрация фракции кодом (из конструкторов систем, до загрузки рангов).
    // id > 0 и уникален; на него завязаны ранги и члены в БД. supervisorId —
    // фракция-куратор: её лидер управляет этой фракцией всегда, ранги — если
    // лидер куратора передал её им в управление (managed ранга).
    void registerFaction(int factionId, std::string name, int supervisorId = NO_FACTION);
    // Расширение маски организации своим битом (>= FIRST_CUSTOM_BIT); имя
    // видно лидеру в меню ранга.
    void registerPermission(int factionId, PermissionMask mask, std::string name);
    // База организации (из конструктора системы конкретной фракции, до
    // initialize — пикапы создаёт FactionSystem).
    void registerBase(int factionId, const Base &base);
    // Точка спавна организации (применяет членам FactionSystem).
    void registerSpawn(int factionId, const Spawn &spawn);
    // Цвет организации (ники/маркеры членов красит FactionSystem).
    void registerColour(int factionId, Colour colour);
    // Пул скинов организации (невалидные id отбрасываются с warning).
    void registerSkins(int factionId, std::vector<int> skins);
    // Скин из пула фракции игрока + право PERM_SKIN у игрока.
    bool canUseSkin(int playerId, int skin) const;
    // Скин организации, который следует НАДЕТЬ члену: сохранённый — если он
    // валиден и есть в пуле; иначе первый из пула. -1 — у организации нет пула
    // (надевать нечего, напр. банки). savedSkin 0/невалидный/не из пула трактуется
    // как «не задан» -> фолбэк на первый из пула.
    int resolveOrgSkin(int factionId, int savedSkin) const;

    const Faction *getFaction(int factionId) const;
    const std::vector<Faction> &getFactions() const;
    const Rank *getRank(int factionId, std::int64_t rankId) const;
    const Rank *defaultRank(int factionId) const; // ранг «Без ранга»
    // Все права организации для UI: 3 базовых + её собственные.
    std::vector<PermissionDef> permissionsOf(int factionId) const;

    // --- членство (игроки онлайн) ---
    int getMemberFaction(int playerId) const; // NO_FACTION — не во фракции
    const Rank *getMemberRank(int playerId) const;
    std::int64_t getMemberSalary(int playerId) const; // 0 — не во фракции
    // Сохранённый выбор скина организации (0 — не задан). Применять надо через
    // resolveOrgSkin: член всегда в скине организации, пока состоит.
    int getMemberSkin(int playerId) const;
    bool isLeader(int playerId) const;
    // Все биты mask есть у ранга игрока; у лидера — всегда true.
    bool hasPermission(int playerId, PermissionMask mask) const;

    // --- операции членства (write-through в БД) ---
    // Требуют активной сессии у игрока. false — невалидная фракция/ранг/сессия.
    bool setMember(IPlayer &player, int factionId, std::int64_t rankId, bool leader, std::int64_t salary);
    bool removeMember(IPlayer &player);
    bool setMemberRank(IPlayer &player, std::int64_t rankId);
    bool setMemberSalary(IPlayer &player, std::int64_t salary); // 0..MAX_SALARY
    // Сохранить выбор скина организации (память + БД write-through). false —
    // не член / скин не из пула организации (валидация против серверных фактов).
    bool setMemberSkin(IPlayer &player, int skin);

    // --- надзор (гос-вертикаль, write-through в БД) ---
    // Игрок управляет фракцией, если он лидер её куратора, либо его ранг в
    // кураторе получил её в управление.
    bool canManage(int playerId, int factionId) const;
    std::vector<const Faction *> managedBy(int playerId) const;      // для UI куратора
    std::vector<const Faction *> subordinatesOf(int factionId) const; // подопечные фракции
    // Передать/забрать подопечную организацию рангу куратора (правка лидера).
    // false — подопечная не курируется этой фракцией / нет ранга.
    bool editRankScope(int factionId, std::int64_t rankId, int subordinateId, bool enabled);
    // Назначить лидера: прежний лидер (в БД и онлайн) становится обычным членом.
    // Цель — не во фракции (вступит на низший ранг) или уже член этой фракции
    // (ранг сохраняется). Член ЧУЖОЙ фракции — отказ.
    bool appointLeader(IPlayer &target, int factionId, std::int64_t salary);
    bool dismissLeader(IPlayer &target); // лидер -> обычный член своей фракции
    // Назначение по аккаунту (работает и для ОФФЛАЙН цели — итог выборов):
    // прежний лидер снимается, цель принудительно переводится из своей фракции
    // (если была в другой) на стартовый ранг новой. onlinePlayer — игрок этого
    // аккаунта, если он в сети (резолвит вызывающий через PlayerSessionService),
    // иначе nullptr.
    bool appointLeaderByAccount(AccountId accountId, int factionId, IPlayer *onlinePlayer);
    int onlineLeaderId(int factionId) const; // playerId лидера онлайн или -1

    // --- бюджет фракции (write-through в БД) ---
    std::int64_t getBudget(int factionId) const;
    bool setBudget(int factionId, std::int64_t amount);  // админка/дев, 0..MAX_BUDGET
    bool deposit(int factionId, std::int64_t amount);    // доходы (насыщается у MAX_BUDGET)
    bool tryWithdraw(int factionId, std::int64_t amount); // false — не хватает

    // --- приказ о выплате зарплат ---
    // Кулдаун приказов; помечается ТОЛЬКО после успешного списания бюджета —
    // отказ из-за пустого бюджета не сжигает попытку (лидер пополнит и повторит).
    bool isPayOrderReady(int factionId, TimePoint now) const;
    void markPayOrderIssued(int factionId, TimePoint now);
    // Сбросить кулдаун в «не было приказов» (TimePoint{}). Нужен для оптимистичной
    // отметки: приказ помечается ДО async-кредита (гасит двойной клик в окне
    // запроса), а если зачислять оказалось нечего/не хватило бюджета — откатываем,
    // чтобы пустая попытка не сжигала кулдаун.
    void clearPayOrderCooldown(int factionId);

    // --- операции лидера с рангами (write-through в БД) ---
    // Имя — utf-8, уже прошедшее sanitizeRankName (пустое — отказ).
    const Rank *createRank(int factionId, const std::string &name);
    bool editRankName(int factionId, std::int64_t rankId, const std::string &name);
    // Биты вне базовых и зарегистрированных для организации — отказ.
    bool editRankPermission(int factionId, std::int64_t rankId, PermissionMask mask, bool enabled);
    // Члены удаляемого ранга (в БД и онлайн) переводятся на «Без ранга».
    // Сам «Без ранга» удалить нельзя.
    bool deleteRank(int factionId, std::int64_t rankId);

    // Вход/выход/появление в сети члена фракции. oldFactionId/newFactionId —
    // NO_FACTION на вступлении/выходе соответственно; на появлении в сети
    // old == NO_FACTION, new == фракция. Выход из игры события НЕ даёт —
    // чистка по дисконнекту остаётся за подписчиком.
    using MemberObserver = std::function<void(IPlayer &, int oldFactionId, int newFactionId)>;
    void subscribeMemberChange(MemberObserver observer);

    // Чистка клиентского ввода имени ранга: управляющие символы (включая \t —
    // разделитель колонок tablist-диалогов) выбрасываются, края обрезаются от
    // пробелов, длина ограничивается по границе utf-8 символа.
    static std::string sanitizeRankName(std::string_view raw);

  private:
    struct Member
    {
        AccountId accountId = PlayerSessionService::NO_ACCOUNT; // есть у всех с сессией
        int factionId = NO_FACTION;
        std::int64_t rankId = 0;
        std::int64_t salary = 0;
        bool leader = false;
        int skin = 0; // выбранный скин организации (0 — не задан -> первый из пула)
    };

    // --- вызывается FactionSystem ---
    // Ранги из БД на старте: прикрепляются к зарегистрированным фракциям,
    // ранги незарегистрированных фракций отбрасываются с warning. Фракции без
    // стартового ранга получают «Без ранга» (создаётся и в БД).
    void loadRanks(std::vector<std::pair<int, Rank>> ranks);
    // Скоупы рангов (rank_id -> подопечная фракция) — строго после loadRanks.
    void loadScopes(std::vector<std::pair<std::int64_t, int>> scopes);
    void loadBudgets(std::vector<std::pair<int, std::int64_t>> budgets);
    void handleSessionStart(IPlayer &player, AccountId accountId, int factionId, std::int64_t rankId, bool leader,
                            std::int64_t salary, int skin);
    void resetPlayer(int playerId);

    Faction *findFaction(int factionId);
    Rank *findRank(int factionId, std::int64_t rankId);
    Rank *findRankById(std::int64_t rankId); // по всем фракциям (загрузка скоупов)
    void notifyChange(IPlayer &player, int oldFactionId, int newFactionId);
    void writeBudget(int factionId, std::int64_t budget);

    std::vector<Faction> m_factions;
    std::int64_t m_nextRankId = 1; // сервер — единственный писатель рангов
    std::array<Member, MAX_PLAYERS> m_members;
    std::vector<MemberObserver> m_observers;
};
