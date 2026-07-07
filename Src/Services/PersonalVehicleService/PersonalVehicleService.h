#pragma once

#include "Macro.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "types.hpp"
#include <array>
#include <string>
#include <tuple>
#include <utility>
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
//
// Внешний вид (цвет/пейнтджоб/компоненты) персистится ТЕМ ЖЕ паттерном, что fuel:
// OwnedVehicle хранит снимок, источник правды которого — либо память (машина не
// заспавнена), либо живой экземпляр VehicleService (заспавнена). Снимается перед
// каждым исчезновением экземпляра (пере-спавн/конец сессии/санкционированная
// смерть) и на КАЖДОМ принятом тюнинге (см. PersonalVehicleSystem::onVehicleTuned)
// — переживает краш до штатного деспавна. Применяется ОДИН РАЗ, в spawn().
class PersonalVehicleService final : public IService
{
    friend PersonalVehicleSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    // Дефолтный лимит личных машин на игрока. Расширяемо позже (премиум/перки) —
    // тогда лимит станет пер-игроковым, m_owned уже vector ради этого.
    static constexpr int MAX_PERSONAL_VEHICLES = 2;

    // Одна запись владения: модель машины + id её текущего заспавненного экземпляра
    // в VehicleService (-1 — не заспавнена сейчас; владение всё равно живёт).
    // dbId — стабильный ключ строки personal_vehicle (id), по которому шеринг семье
    // ссылается на КОНКРЕТНУЮ машину. -1 — ещё не присвоен (окно между buy и приходом
    // LAST_INSERT_ID из async-INSERT); такую машину нельзя расшарить, пока id не лёг.
    // fuel — ПЕРСИСТЕНТНЫЙ остаток бака (0..VehicleService::FUEL_CAPACITY):
    // источник правды, пока машина НЕ заспавнена (vehicleId == -1); пока заспавнена —
    // источник правды VehicleService::getFuel(vehicleId), это поле лишь снимок на
    // момент последнего сохранения (captureFuel перед уничтожением/сессией).
    //
    // colour1/colour2/paintJob/components — ПЕРСИСТЕНТНЫЙ снимок внешнего вида, та
    // же модель, что fuel: источник правды, пока машина НЕ заспавнена; пока
    // заспавнена — источник правды живой экземпляр VehicleService (getColour/
    // getPaintJob/getComponents), поля лишь снимок на момент последнего сохранения.
    // colour1 == -1 — «не сохранено» (только что куплена/не загружено из БД):
    // spawn() трактует как «спавнить рандомным цветом» (colour2 в этом случае
    // тоже -1 — пара неразделима). paintJob == -1 — «нет пейнтджоба» (совпадает с
    // тем, что отдаёт голое SDK для машины без пейнтджоба — не нужен отдельный
    // сентинел). components — набор id установленных компонентов (до
    // VehicleService::COMPONENT_SLOT_COUNT штук, по одному на слот).
    struct OwnedVehicle
    {
        int model = 0;
        int vehicleId = -1;
        long long dbId = -1;
        float fuel = -1.0f; // -1 — не загружено из БД/не сохранено; spawn() трактует как полный бак
        int colour1 = -1;   // -1 — не сохранено, спавнить рандомным цветом
        int colour2 = -1;
        int paintJob = -1; // -1 — нет пейнтджоба
        std::vector<int> components;
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

    // Купить личную машину игроку — регистрирует ТОЛЬКО ВЛАДЕНИЕ (модель) в ПАМЯТИ
    // (оптимистично, как FactionService::setMember). Машину НЕ создаёт (спавн — через
    // парковку) и В БД НЕ ПИШЕТ: write-through INSERT делает вызывающая система
    // (PersonalVehicleSystem) через selectQuery<LAST_INSERT_ID>, чтобы вернуть dbId и
    // проставить его в запись (setDbId). Гейт loaded (NotLoaded, пока зеркало из БД не
    // легло), лимит и модель валидируются ЗДЕСЬ (клиенту не доверяем). accountId —
    // серверный (из сессии), не от клиента. При Ok добавляет OwnedVehicle{model,-1,-1}
    // и, если outIndex != nullptr, пишет туда индекс добавленной записи (для колбэка
    // проставления dbId).
    BuyResult buy(int playerId, AccountId accountId, int model, int *outIndex = nullptr);

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
    // старый уничтожается, но СНАЧАЛА его fuel (VehicleService::getFuel) снимается в
    // entry.fuel (переживает пере-спавн — иначе вызов машины заново доливал бы бак
    // бесплатно), новый создаётся на точке. Машина — через VehicleService::create,
    // ЦВЕТ которой берётся из сохранённого снимка (entry.colour1 >= 0 — сохранённая
    // пара colour1/colour2; иначе — переданные аргументы colour1/colour2, обычно
    // -1/-1 = рандом ядра, как их сегодня передаёт ParkingSystem). Затем
    // ПРИМЕНЯЮТСЯ пейнтджоб (entry.paintJob >= 0) и компоненты (entry.components,
    // каждый addComponent) — серверные вызовы, не клиентские SCM-заявки, анти-чит
    // мод-шопа их не видит (см. VehicleService::addComponent). Далее ПРИМЕНЯЕТСЯ
    // персистентный fuel записи (entry.fuel >= 0 — сохранённый остаток; -1 — ещё не
    // сохранён/только куплена, create уже дал полный бак по дефолту, setFuel не
    // зовём). При Ok записывает новый id в владение и отдаёт *out. Запись владения
    // НЕ удаляется ни на одной ветке — индекс стабилен.
    SpawnResult spawn(int playerId, int ownedIndex, Vector3 position, float angle, int colour1, int colour2,
                      IVehicle **out = nullptr);

    // id текущего заспавненного экземпляра владения (или -1) — для исключения своей
    // машины при поиске свободной точки на пере-спавне. Bounds-safe.
    int currentVehicle(int playerId, int ownedIndex) const;

    // dbId записи ownedIndex игрока (или -1). Для персиста fuel вызывающей системой
    // (PersonalVehicleSystem — write-through UPDATE по dbId). Bounds-safe.
    long long dbIdOf(int playerId, int ownedIndex) const;

    // Записать ПЕРСИСТЕНТНЫЙ снимок остатка топлива записи ownedIndex (кламп
    // 0..VehicleService::FUEL_CAPACITY; NaN/Inf игнорируется). Только ПАМЯТЬ — write-
    // through делает вызывающая система. Для: (1) загрузки из БД (через load), (2)
    // снимка перед уничтожением заспавненного экземпляра (пере-спавн/disconnect/
    // санкционированная смерть), (3) восстановления после респавна несанкционированной
    // смерти. Bounds-safe; no-op для несуществующей записи.
    void setFuel(int playerId, int ownedIndex, float fuel);
    // Текущий ПЕРСИСТЕНТНЫЙ снимок fuel записи (или -1 для bounds-промаха/невалидной
    // записи — трактуется вызывающим как «нет сохранённого, дефолт полный бак»).
    float fuelOf(int playerId, int ownedIndex) const;

    // Записать ПЕРСИСТЕНТНЫЙ снимок внешнего вида (цвет+пейнтджоб+компоненты) записи
    // ownedIndex. Только ПАМЯТЬ — write-through делает вызывающая система. Компоненты
    // копируются как есть (валидация диапазона id — на вызывающей стороне при
    // загрузке из БД; снимок с живого экземпляра уже валиден по построению). colour1
    // < 0 — трактуется как «нет сохранённого цвета» (colour2 форсится в -1 вместе с
    // ним — пара неразделима, не бывает наполовину сохранённого цвета). Bounds-safe;
    // no-op для несуществующей записи.
    void setAppearance(int playerId, int ownedIndex, int colour1, int colour2, int paintJob,
                       const std::vector<int> &components);
    // Снимок внешнего вида записи (для write-through персиста вызывающей системой).
    // Возвращает указатель на запись (nullptr для bounds-промаха) — читать сразу все
    // поля без четырёх раздельных геттеров; вызывающий не должен хранить указатель
    // дольше одного колбэка (владение может измениться).
    const OwnedVehicle *appearanceOf(int playerId, int ownedIndex) const;

    // Сериализовать набор компонентов в JSON-массив id (для write-through UPDATE
    // одной строкой, personal_vehicle.components) — тот же формат, что парсит load()
    // из БД. Общий хелпер, чтобы формат сериализации/десериализации не разъезжался
    // между системой (пишет) и сервисом (читает).
    static std::string componentsToJson(const std::vector<int> &components);

    // Число владений игрока (bounds-safe; 0 для невалидного id).
    int count(int playerId) const;
    // Достигнут ли лимит игроком (bounds-safe; false для невалидного id).
    bool atLimit(int playerId) const;

    // --- ре-тег парковки НА МЕСТЕ (зовёт CarMenuSystem, поиск по dbId) ---
    // Отвязать владение (по dbId) от сессионного трекинга: vehicleId -> -1 БЕЗ destroy.
    // Для парковки НА МЕСТЕ: живой экземпляр остаётся (его держит ParkedVehicleService),
    // но reset() на дисконнекте владельца его больше НЕ уничтожит (иначе припаркованная
    // машина гибла бы). Ре-тег destroy НЕ зовёт -> onWorldVehicleDestroyed не приходит
    // -> vehicleId сам не обнуляется, detach делается ЯВНО. Идемпотентно; bounds-safe.
    // Возвращает true, если владение с таким dbId найдено.
    bool detach(long long dbId);

    // Вернуть владение (по dbId) в сессионный трекинг: vehicleId -> vehicleId. Для снятия
    // с парковки: машина снова обычная личная сессионная (уничтожается на дисконнекте/
    // смерти, как до парковки). bounds-safe. Возвращает true, если владение найдено.
    bool attach(long long dbId, int vehicleId);

  private:
    // --- вызывается PersonalVehicleSystem ---
    // Загрузка владения по старту сессии: кладёт строки (dbId, модель, fuel, colour1,
    // colour2, paintJob, componentsJson) из БД в память как OwnedVehicle{...} (БЕЗ
    // записи в БД — это зеркало, не покупка) и поднимает per-player флаг loaded
    // (покупка разблокирована). fuel клампится 0..CAP (мусор из БД — NaN/
    // отрицательное/сверх капасити). Цвет/пейнтджоб/компоненты валидируются перед
    // укладкой в память (см. .cpp) — мусор из БД не должен доехать до SDK на spawn.
    // componentsJson — JSON-массив id компонентов (nlohmann, парс в try/catch);
    // невалидный JSON/элемент вне диапазона — пропускается. Память перед этим уже
    // пуста (reset на коннекте/конце прошлой сессии). Bounds-safe.
    void load(int playerId,
             const std::vector<std::tuple<long long, int, double, int, int, int, std::string>> &rows);

    // Проставить dbId владению ownedIndex игрока (success-колбэк async-INSERT покупки).
    // Пишет только если запись существует и её dbId ещё -1 (не перетереть уже
    // присвоенный — на случай reload/reset между запуском и колбэком). Bounds-safe.
    void setDbId(int playerId, int ownedIndex, long long dbId);

    // Конец/начало сессии: уничтожить ВСЕ заспавненные машины игрока, очистить
    // ПАМЯТЬ владения и снять флаг loaded. БД НЕ ТРОГАЕМ — право владения остаётся в
    // personal_vehicle и подтянется на следующем старте. Bounds-safe. ВАЖНО:
    // персист fuel (write-through UPDATE) — забота ВЫЗЫВАЮЩЕЙ системы ДО этого
    // вызова (снять getFuel живых экземпляров, пока они ещё существуют) — reset сам
    // БД не трогает и стирает память владения безусловно.
    void reset(int playerId);

    // Машину уничтожил кто-то ещё (пул-событие через VehicleService::subscribeDestroyed
    // — взрыв/респаун-цикл/destroy): найти владение с этим vehicleId (по всем игрокам)
    // и обнулить его vehicleId. ВЛАДЕНИЕ НЕ УДАЛЯЕТСЯ (машина пропала, спавнят заново
    // через парковку). НЕ зовёт destroy. Bounds-safe; линейный проход (мало).
    void onWorldVehicleDestroyed(int vehicleId);

    VehicleService *m_vehicleService = nullptr; // источник правды о машинах (bind)

    // Владение per-player: модель + id текущего экземпляра. vector ради будущего
    // расширения лимита; при MAX_PERSONAL_VEHICLES==2 держит 0..2 элемента.
    std::array<std::vector<OwnedVehicle>, MAX_PLAYERS> m_owned;

    // Гейт старт-гонки «купил до загрузки»: пока зеркало владения из БД не легло в
    // m_owned, buy отдаёт NotLoaded (иначе купленная модель + загруженная обошли бы
    // лимит). Ставится load, снимается reset.
    std::array<bool, MAX_PLAYERS> m_loaded{};
};
