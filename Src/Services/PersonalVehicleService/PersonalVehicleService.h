#pragma once

#include "Macro.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "types.hpp"
#include <array>
#include <vector>

class PersonalVehicleSystem;

// Личный транспорт игрока — бизнес-фича (НЕ Core). Источник правды о ВЛАДЕНИИ
// личными машинами игрока ОНЛАЙН: какие МОДЕЛИ принадлежат аккаунту. Само ПРАВО
// ВЛАДЕНИЯ ПЕРСИСТИТСЯ в БД (personal_vehicle, по account_id, write-through — как
// членство фракций): грузится на старте сессии (load) и переживает перезаход.
// А вот машина-СУЩНОСТЬ в мире — СЕССИОННАЯ: спавнится/уничтожается через парковку
// (ParkingSystem), её позиция/состояние НЕ сохраняются (vehicleId обнуляется на
// reset в конце сессии).
//
// Разделение «владею vs вызываю» (как дом: владение vs вход): покупка регистрирует
// ВЛАДЕНИЕ (модель машины) — память + write-through INSERT в БД, машину НЕ спавнит;
// спавн — отдельный шаг (через парковку, ParkingSystem). Уничтожение машины НЕ
// снимает владение, лишь обнуляет её id — её спавнят заново через парковку.
//
// Машины создаются ТОЛЬКО через VehicleService::create (тег Owner::Player, ownerId
// = playerId — серверный сессионный ключ); сырой SDK сервис не трогает. Лимит на
// число машин и валидация модели форсятся ЗДЕСЬ (серверная политика), не на клиенте.
// Per-player флаг loaded гейтит покупку до прихода зеркала из БД: иначе «купил до
// загрузки» обошёл бы лимит (загрузка добавила бы вторую модель поверх купленной).
class PersonalVehicleService final : public IService
{
    friend PersonalVehicleSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    // Дефолтный лимит личных машин на игрока. Расширяемо позже (премиум/перки) —
    // тогда лимит станет пер-игроковым, m_owned уже vector ради этого.
    static constexpr int MAX_PERSONAL_VEHICLES = 1;

    // Одна запись владения: модель машины + id её текущего заспавненного экземпляра
    // в VehicleService (-1 — не заспавнена сейчас; владение всё равно живёт).
    struct OwnedVehicle
    {
        int model = 0;
        int vehicleId = -1;
    };

    // Привязать VehicleService (источник правды о машинах). Зовётся
    // PersonalVehicleSystem в конструкторе — реестр создаёт сервисы дефолтным
    // ctor, зависимости проставляются здесь (как VehicleService::bind).
    void bind(VehicleService &vehicleService);

    // Итог покупки: код для сообщения вызывающему (тексты — в системе).
    enum class BuyResult
    {
        Ok,
        LimitReached, // достигнут MAX_PERSONAL_VEHICLES
        InvalidModel, // модель вне диапазона машин SA (400..611) либо train carriage (569/570)
        PoolFull,     // не используется (buy больше не спавнит); ветка оставлена для совместимости
        NotLoaded,    // владение аккаунта ещё грузится из БД (loaded=false) — покупка отложена
        Unavailable   // нет игрока/сервис не связан (внутреннее предусловие; из штатной команды недостижимо)
    };

    // Купить личную машину игроку — регистрирует ТОЛЬКО ВЛАДЕНИЕ (модель): память +
    // write-through INSERT в БД (personal_vehicle по accountId). Машину НЕ создаёт
    // (спавн — через парковку). Гейт loaded (NotLoaded, пока зеркало из БД не легло),
    // лимит и модель валидируются ЗДЕСЬ (клиенту не доверяем). accountId — серверный
    // (из сессии), не от клиента. При Ok добавляет OwnedVehicle{model, -1}.
    BuyResult buy(int playerId, AccountId accountId, int model);

    // Список владений игрока (для диалога парковки; bounds-safe — пустой статический
    // для невалидного id).
    const std::vector<OwnedVehicle> &owned(int playerId) const;

    // Итог спавна на парковке: код для сообщения вызывающему.
    enum class SpawnResult
    {
        Ok,
        BadIndex,    // невалидный playerId/ownedIndex
        PoolFull,    // пул машин ядра исчерпан (create вернул nullptr)
        Unavailable  // сервис не связан с VehicleService
    };

    // Заспавнить владение ownedIndex игрока на заданной точке (серверные координаты
    // от парковки). Если экземпляр уже заспавнен (vehicleId != -1) — ПЕРЕ-СПАВН:
    // старый destroy, новый на точке. Машина — через VehicleService::create
    // (Owner::Player, ownerId=playerId). При Ok записывает новый id в владение и
    // отдаёт *out. Запись владения НЕ удаляется ни на одной ветке — индекс стабилен.
    SpawnResult spawn(int playerId, int ownedIndex, Vector3 position, float angle, int colour1, int colour2,
                      IVehicle **out = nullptr);

    // id текущего заспавненного экземпляра владения (или -1) — для исключения своей
    // машины при поиске свободной точки на пере-спавне. Bounds-safe.
    int currentVehicle(int playerId, int ownedIndex) const;

    // Число владений игрока (bounds-safe; 0 для невалидного id).
    int count(int playerId) const;
    // Достигнут ли лимит игроком (bounds-safe; false для невалидного id).
    bool atLimit(int playerId) const;

  private:
    // --- вызывается PersonalVehicleSystem ---
    // Загрузка владения по старту сессии: кладёт модели из БД в память как
    // OwnedVehicle{model, -1} (БЕЗ записи в БД — это зеркало, не покупка) и поднимает
    // per-player флаг loaded (покупка разблокирована). Память перед этим уже пуста
    // (reset на коннекте/конце прошлой сессии). Bounds-safe.
    void load(int playerId, const std::vector<int> &models);

    // Конец/начало сессии: уничтожить ВСЕ заспавненные машины игрока, очистить
    // ПАМЯТЬ владения и снять флаг loaded. БД НЕ ТРОГАЕМ — право владения остаётся в
    // personal_vehicle и подтянется на следующем старте. Bounds-safe.
    void reset(int playerId);

    // Машину уничтожил кто-то ещё (пул-событие через VehicleService::subscribeDestroyed
    // — взрыв/респаун-цикл/destroy): найти владение с этим vehicleId (по всем игрокам)
    // и обнулить его vehicleId. ВЛАДЕНИЕ НЕ УДАЛЯЕТСЯ (машина пропала, спавнят заново
    // через парковку). НЕ зовёт destroy. Bounds-safe; линейный проход (мало).
    void onWorldVehicleDestroyed(int vehicleId);

    VehicleService *m_vehicleService = nullptr; // источник правды о машинах (bind)

    // Владение per-player: модель + id текущего экземпляра. vector ради будущего
    // расширения лимита; при MAX_PERSONAL_VEHICLES==1 держит 0..1 элемент.
    std::array<std::vector<OwnedVehicle>, MAX_PLAYERS> m_owned;

    // Гейт старт-гонки «купил до загрузки»: пока зеркало владения из БД не легло в
    // m_owned, buy отдаёт NotLoaded (иначе купленная модель + загруженная обошли бы
    // лимит). Ставится load, снимается reset.
    std::array<bool, MAX_PLAYERS> m_loaded{};
};
