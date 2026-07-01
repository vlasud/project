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
// РАСШАРЕННЫХ) и спавнится у дома независимо от онлайна владельца.
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
    };

    // Итог операции: код для сообщения вызывающему (тексты — в системе).
    enum class Result
    {
        Ok,
        AlreadyParked, // машина уже припаркована (dbId в m_byDbId)
        NotParked,     // машина не припаркована (снятие/запрос по несуществующему dbId)
        AlreadyShared, // при shareToFamily: уже расшарена (familyId != NO_FAMILY)
        NotShared,     // при unshareFromFamily: не расшарена (familyId == NO_FAMILY)
        Invalid        // невалидный dbId/сервис не связан
    };

    // Привязать зависимости (реестр создаёт сервис дефолтным ctor; bind — в
    // конструкторе ParkedVehicleSystem, как PersonalVehicleService::bind). HouseService
    // здесь НЕ нужен: дом-гейт живёт в системе (где есть игрок/позиция).
    void bind(VehicleService &vehicleService, FamilyService &familyService);

    // --- стартовая загрузка (из parked_vehicle, строго после семей) ---
    // Зарегистрировать парковку из БД в памяти (БЕЗ записи — это зеркало). vehicleId
    // проставит система после create через setVehicleId. familyId==NO_FAMILY — личная.
    void loadParked(long long dbId, AccountId owner, int model, Vector3 spot, float angle, int familyId);

    // --- операции (write-through в parked_vehicle) ---
    // Припарковать машину лично (familyId=NO_FAMILY): регистрирует Parked в памяти +
    // INSERT. Живой экземпляр (create Owner::Parked) заводит система и связывает
    // setVehicleId.
    Result park(long long dbId, AccountId owner, int model, Vector3 spot, float angle);
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
    // Режим размещения: -1 — не припаркована; NO_FAMILY — личная у дома; иначе familyId
    // расшаренной. Единый словарь статусов /car/парковки читает отсюда.
    int parkedMode(long long dbId) const;
    std::vector<long long> parkedByAccount(AccountId accountId) const;
    // Число припаркованных машин аккаунта (личные + расшаренные — все в m_byAccount).
    // Для капа парковки у дома (CarMenuSystem) — без аллокации вектора. O(1) в среднем.
    std::size_t countParkedByAccount(AccountId accountId) const;
    // Расшаренные семье машины. Пусто для NO_FAMILY (личные в m_byFamily не лежат).
    std::vector<long long> parkedOfFamily(int familyId) const;
    const Parked *byDbId(long long dbId) const;
    const Parked *byVehicleId(int vehicleId) const;

    // Гейт доступа за руль. Не-Parked машина — всегда true (гейт не наш). Личная
    // (familyId==NO_FAMILY) — только владелец (accountId == ownerAccountId, accountId
    // серверный, NO_ACCOUNT не проходит). Расшаренная — член семьи familyId. O(1).
    bool canDrive(int vehicleId, AccountId accountId) const;

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

    VehicleService *m_vehicleService = nullptr;
    FamilyService *m_familyService = nullptr;

    std::unordered_map<long long, Parked> m_byDbId;            // dbId -> Parked (владеющий)
    std::unordered_map<int, long long> m_byVehicleId;          // vehicleId -> dbId
    std::unordered_multimap<int, long long> m_byFamily;        // familyId -> dbId (без NO_FAMILY)
    std::unordered_multimap<AccountId, long long> m_byAccount; // ownerAccountId -> dbId
    std::vector<ReconcileObserver> m_reconcileObservers;
};
