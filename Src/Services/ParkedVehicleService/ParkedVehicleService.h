#pragma once

#include "Macro.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/IService.h"
#include "types.hpp"
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <vector>

class ParkedVehicleSystem;

// Припаркованные у дома машины — бизнес-фича (НЕ Core). Источник правды о том, какие
// ЛИЧНЫЕ машины припаркованы у дома владельца. Парковка — БАЗОВОЕ состояние машины у
// дома (постоянная точка ≤30 м, персист, переживает выход/рестарт); шеринг семье —
// РЕЖИМ ДОСТУПА поверх той же припаркованной машины (familyId != NO_FAMILY). Обе
// разновидности — один тег VehicleService (Owner::Parked); собственность остаётся у
// аккаунта (запись personal_vehicle НЕ трогается). Персист в parked_vehicle
// (write-through), грузится на старте СТРОГО после семей (семьи нужны для гейта
// РАСШАРЕННЫХ). Живой экземпляр у точки НЕ ждёт никого: он существует только пока
// машина ВЫЗВАНА (call) и уходит из мира, когда вызвавший вышел.
//
// Живой экземпляр в мире — Owner::Parked. При смерти он НЕ уничтожается
// (PersonalVehicleSystem::onVehicleDied трогает только Owner::Player), ядро само
// переспавнит его на spawn-позиции = spot. Парковка/снятие идут ре-тегом НА МЕСТЕ (тот
// же vehicleId, без destroy/create): CarMenuSystem меняет серверный тег
// (VehicleService::setOwner) и spawn-точку (setSpawnPosition) на ЖИВОМ экземпляре, park
// лишь регистрирует запись+INSERT, unparkKeepInstance снимает запись+DELETE, экземпляр
// не трогая. Штатный destroy живого экземпляра — только через unpark (пул-очистка).
// Переходы режима доступа (shareToFamily/unshareFromFamily) экземпляр НЕ трогают —
// только UPDATE family_id (замок применится на СЛЕДУЮЩЕЙ посадке за руль).
//
// Индексы (все O(1)/холодные): по dbId (PK парковки), по vehicleId (гейт доступа и
// снятие), по ownerAccountId (крайние случаи/parkedByAccount) и по familyId
// (расшаренные — крайние случаи роспуска/parkedOfFamily). NO_FAMILY (личная) в
// m_byFamily НЕ индексируется.
class ParkedVehicleService final : public IService
{
    friend ParkedVehicleSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    struct Parked
    {
        long long personalVehicleId = -1; // dbId (PK; ссылка на personal_vehicle.id)
        AccountId ownerAccountId = PlayerSessionService::NO_ACCOUNT;
        int model = 0;
        Vector3 spot{};   // точка спавна у дома (абсолютная — переживает потерю дома)
        float angle = 0.0f;
        // NO_FAMILY = личная owner-only; иначе = расшарена этой семье (режим доступа).
        int familyId = FamilyService::NO_FAMILY;
        int vehicleId = -1; // id живого экземпляра (Owner::Parked) или -1
        // ПЕРСИСТЕНТНЫЙ снимок остатка бака (0..VehicleService::FUEL_CAPACITY):
        // источник правды, пока экземпляр НЕ заспавнен (vehicleId == -1); пока
        // заспавнен — источник правды VehicleService::getFuel(vehicleId), это поле
        // лишь снимок на момент последнего сохранения (перед despawn/детонацией).
        float fuel = VehicleService::FUEL_CAPACITY;
        // Кто вызвал машину к месту парковки. NO_ACCOUNT — не вызвана, и тогда
        // живого экземпляра в мире НЕТ. Состояние РАНТАЙМНОЕ и в БД не уходит:
        // после рестарта сервера в мире не ждёт ничего, пока не вызовут заново.
        AccountId calledBy = PlayerSessionService::NO_ACCOUNT;
    };

    // Итог операции: код для сообщения вызывающему (тексты — в системе).
    enum class Result
    {
        Ok,
        AlreadyParked, // машина уже припаркована (dbId в m_byDbId)
        NotParked,     // машина не припаркована (снятие/запрос по несуществующему dbId)
        AlreadyShared, // при shareToFamily: уже расшарена (familyId != NO_FAMILY)
        NotShared,     // при unshareFromFamily: не расшарена (familyId == NO_FAMILY)
        Invalid,       // невалидный dbId/сервис не связан
        NoInstance,    // при respawnHome: экземпляр сейчас не в мире (vehicleId == -1)
        Occupied,      // при respawnHome: за рулём есть водитель — респавн под ним недопустим
        NoAccess       // при call: вызывающий не владелец и не член семьи-получателя
    };

    // Привязать зависимости (реестр создаёт сервис дефолтным ctor; bind — в
    // конструкторе ParkedVehicleSystem, как PersonalVehicleService::bind). HouseService
    // здесь НЕ нужен: дом-гейт живёт в системе (где есть игрок/позиция).
    void bind(VehicleService &vehicleService, FamilyService &familyService);

    // --- стартовая загрузка (из parked_vehicle, строго после семей) ---
    // Зарегистрировать парковку из БД в памяти (БЕЗ записи — это зеркало). vehicleId
    // проставит система после create через setVehicleId. familyId==NO_FAMILY — личная.
    // fuel — персистентный остаток бака, клампится 0..VehicleService::FUEL_CAPACITY
    // (мусор из БД — NaN/отрицательное/сверх капасити).
    void loadParked(long long dbId, AccountId owner, int model, Vector3 spot, float angle, int familyId, float fuel);

    // --- операции (write-through в parked_vehicle) ---
    // Припарковать машину лично (familyId=NO_FAMILY): регистрирует Parked в памяти +
    // INSERT и сразу связывает запись с ЖИВЫМ экземпляром liveVehicleId, который
    // игрок только что пригнал (парковка = ре-тег НА МЕСТЕ, машину не создаём).
    // Экземпляр уже стоит у точки, поэтому запись помечается ВЫЗВАННОЙ владельцем:
    // без этого calledBy == NO_ACCOUNT при живом vehicleId разошёлся бы с миром, и
    // машина осталась бы ждать у точки после выхода владельца. Слот вызова один —
    // прошлая вызванная владельцем машина снимается, как и при call(). fuel —
    // снимок остатка бака НА МОМЕНТ парковки (топливо у экземпляра уже то, что
    // наездил игрок; INSERT обязан записать РЕАЛЬНЫЙ остаток, не дефолт БД).
    Result park(long long dbId, AccountId owner, int model, Vector3 spot, float angle, float fuel,
                int liveVehicleId);
    // Убрать с парковки, УНИЧТОЖИВ живой экземпляр (через VehicleService): удаляет
    // Parked из памяти + DELETE строки. Для путей, где машина должна ИСЧЕЗНУТЬ вместе с
    // парковкой (напр. пул-очистка). Централизует «destroy + БД» — строка не осиротеет.
    Result unpark(long long dbId);
    // Убрать с парковки, СОХРАНИВ живой экземпляр: удаляет Parked из памяти + DELETE
    // строки, но экземпляр в мире НЕ трогает (destroy НЕ зовётся). Для снятия НА МЕСТЕ:
    // машина остаётся стоять, её берёт под себя PersonalVehicleService (снова личная
    // сессионная). Обратная ссылка на vehicleId снимается из индекса (eraseFromMemory).
    Result unparkKeepInstance(long long dbId);

    // --- режим доступа БЕЗ пересоздания машины (write-through UPDATE family_id) ---
    // Расшарить припаркованную машину семье: NO_FAMILY -> familyId. Экземпляр НЕ
    // трогается — меняется только гейт доступа (замок применится на следующей посадке).
    Result shareToFamily(long long dbId, int familyId);
    // Снять шеринг: familyId -> NO_FAMILY. Машина ОСТАЁТСЯ припаркованной ЛИЧНО у дома
    // (экземпляр не уничтожается). Только UPDATE + правка m_byFamily.
    Result unshareFromFamily(long long dbId);

    // --- «Респавн» из /car для припаркованной у дома/в семье машины ---
    // Вернуть живой экземпляр НА ЕГО ТОЧКУ У ДОМА (spawn-позиция уже = точка
    // парковки — VehicleService::respawn). respawn() ядра ДАЁТ ПОЛНЫЙ БАК
    // (VehicleService::onVehicleRespawn) — АНТИ-АБЬЮЗ: перед вызовом снимается
    // ТЕКУЩИЙ остаток топлива живого экземпляра в саму запись (Parked::fuel —
    // то же поле, источник правды для деспавненной машины);
    // ParkedVehicleSystem::onVehicleRespawned (уже подписан на VehicleService::
    // subscribeRespawned для восстановления после несанкционированной смерти)
    // восстанавливает ЕГО поверх дефолтного полного бака — тот же путь, без
    // дублирования кода восстановления. НЕ уничтожает и не создаёт машину.
    // Отказы: NotParked (нет записи), NoInstance (vehicleId == -1, машина «в гараже»),
    // Occupied (за рулём есть водитель — не выдёргиваем машину из-под сидящего).
    Result respawnHome(long long dbId);

    // --- связь с живым экземпляром ---
    void setVehicleId(long long dbId, int vehicleId); // после create/respawn-цикла
    void onVehicleDestroyed(int vehicleId);           // экземпляр уничтожен — обнулить vehicleId

    // --- наблюдатель приведения желаемого состояния экземпляра ---
    // Запись сменила desired-состояние (флип family_id: share/unshare/крайние случаи) —
    // подписчик приводит живой экземпляр (спавн/деспавн). Тот же паттерн наблюдателей,
    // что у VehicleService/FamilyService: сервис остаётся источником правды о записях,
    // а create/destroy и знание об онлайне владельца живут в системе (ParkedVehicleSystem).
    using ReconcileObserver = std::function<void(long long dbId)>;
    void subscribeReconcile(ReconcileObserver observer);

    // --- запросы (O(1)/холодные) ---
    bool isParked(long long dbId) const;
    // Сентинел «не припаркована» для parkedMode. НЕ -1: FamilyService::NO_FAMILY тоже
    // равен -1, и с общим значением «не припаркована» и «припаркована лично» были бы
    // неразличимы (непри­паркованные показывались «у дома», гейт шеринга отказывал
    // уже припаркованной).
    static constexpr int NOT_PARKED = -2;
    // Радиус «стоит у дома»: живой экземпляр в пределах этого расстояния (по XY) от
    // своей точки spot считается стоящим у дома. Для статуса списков: дальше —
    // машина «брошена» где-то (владелец уехал на ней и вышел).
    static constexpr float HOME_SPOT_RADIUS = 30.0f;
    // Живой экземпляр записи dbId существует И стоит дальше HOME_SPOT_RADIUS от
    // своей точки. false — записи/экземпляра нет (экземпляр вернётся на точку
    // респавном) либо стоит у дома. Только ФАКТ — слово статуса выбирает UI.
    // Позиция машины клиент-авторитетна (см. VehicleService) — влияет ТОЛЬКО на
    // отображаемое владельцу слово, никаких прав не даёт.
    bool isAwayFromSpot(long long dbId) const;
    // Режим размещения: NOT_PARKED — не припаркована; NO_FAMILY — личная у дома;
    // иначе familyId расшаренной. Единый словарь статусов /car/парковки читает отсюда.
    int parkedMode(long long dbId) const;
    std::vector<long long> parkedByAccount(AccountId accountId) const;
    // Число припаркованных машин аккаунта (личные + расшаренные — все в m_byAccount).
    // Для капа парковки у дома (CarMenuSystem) — без аллокации вектора. O(1) в среднем.
    std::size_t countParkedByAccount(AccountId accountId) const;
    // Расшаренные семье машины. Пусто для NO_FAMILY (личные в m_byFamily не лежат).
    std::vector<long long> parkedOfFamily(int familyId) const;
    const Parked *byDbId(long long dbId) const;
    const Parked *byVehicleId(int vehicleId) const;

    // Записать ПЕРСИСТЕНТНЫЙ снимок остатка топлива записи dbId (кламп 0..CAP;
    // NaN/Inf игнорируется). Только ПАМЯТЬ — write-through делает вызывающая система
    // (ParkedVehicleSystem). Для снимка перед деспавном/детонацией и восстановления
    // после респавна несанкционированной смерти. Bounds-safe; no-op для
    // несуществующего dbId.
    void setFuel(long long dbId, float fuel);

    // Гейт доступа за руль. Не-Parked машина — всегда true (гейт не наш). Личная
    // (familyId==NO_FAMILY) — только владелец (accountId == ownerAccountId, accountId
    // серверный, NO_ACCOUNT не проходит). Расшаренная — член семьи familyId. O(1).
    bool canDrive(int vehicleId, AccountId accountId) const;

    // ---------------------------------------------------------------- вызов машины
    //
    // Машина у места парковки в мире НЕ ждёт: живой экземпляр существует только
    // пока машина ВЫЗВАНА. Сервис правит состояние и уведомляет reconcile, а
    // create/destroy делает система (у неё VehicleService и онлайн игроков).

    // Вызвать машину к её месту парковки. Прежняя вызванная ЭТИМ ЖЕ игроком
    // снимается безусловно — даже если в ней кто-то сидит (решение владельца).
    // Вызывать может владелец, а расшаренную семье — любой член этой семьи;
    // расстояние роли не играет (машина появляется на своей точке, не у игрока).
    // Повторный вызов той же машины тем же игроком (она уже в мире) = ПОДАТЬ ЕЁ НА
    // МЕСТО заново: делегирует в respawnHome (тот же путь, что «Респавн», с
    // анти-абьюзом топлива), поэтому может вернуть Occupied — под сидящим водителем
    // машину не выдёргиваем.
    Result call(long long dbId, AccountId caller);

    // Снять вызов: экземпляр уйдёт из мира на reconcile. Идемпотентно.
    void clearCall(long long dbId);

    // Кто вызвал машину (NO_ACCOUNT — не вызвана).
    AccountId calledBy(long long dbId) const;

    // Записи, вызванные этим аккаунтом (обычно ноль или одна).
    std::vector<long long> calledByAccount(AccountId caller) const;

    // Вправе ли аккаунт вызывать эту машину (то же правило, что у доступа за руль).
    bool canCall(long long dbId, AccountId accountId) const;

    // --- крайние случаи (write-through UPDATE family_id) ---
    // Семья распущена: СНЯТЬ ШЕРИНГ у всех её машин (UPDATE family_id -> NO_FAMILY).
    // Машины ОСТАЮТСЯ припаркованы ЛИЧНО у дома (экземпляры не уничтожаются).
    void onFamilyDissolved(int familyId);
    // Владелец машин вышел из семьи: снять шеринг у ЕГО расшаренных машин (UPDATE по
    // каждой с familyId != NO_FAMILY). Личные не трогает.
    void onOwnerLeftFamily(AccountId accountId);

  private:
    // Уничтожить живой экземпляр парковки через VehicleService (bounds/null-safe).
    void destroyInstance(const Parked &parked);
    // Удалить один Parked из всех индексов памяти (БД не трогает).
    void eraseFromMemory(long long dbId);
    // Write-through UPDATE family_id одной строки (share/unshare/крайние случаи).
    void persistFamily(long long dbId, int familyId);
    // Прогнать m_reconcileObservers по dbId (после каждого флипа family_id).
    void notifyReconcile(long long dbId);

    // Снять все вызовы аккаунта, кроме keepDbId: слот вызова у аккаунта ОДИН.
    // Одно место на call() и park() (припаркованная на месте машина — тот же
    // занятый слот). Прошлый экземпляр уничтожается безусловно, в т.ч. с людьми
    // внутри — это решение вызвавшего.
    void releaseOtherCalls(AccountId caller, long long keepDbId);

    // Общее правило доступа: личную водит/вызывает только владелец, расшаренную —
    // любой член семьи-получателя. Одно место на canDrive и canCall.
    bool hasAccess(const Parked &parked, AccountId accountId) const;

    VehicleService *m_vehicleService = nullptr;
    FamilyService *m_familyService = nullptr;

    std::unordered_map<long long, Parked> m_byDbId;            // dbId -> Parked (владеющий)
    std::unordered_map<int, long long> m_byVehicleId;          // vehicleId -> dbId
    std::unordered_multimap<int, long long> m_byFamily;        // familyId -> dbId (без NO_FAMILY)
    std::unordered_multimap<AccountId, long long> m_byAccount; // ownerAccountId -> dbId
    std::vector<ReconcileObserver> m_reconcileObservers;
};
