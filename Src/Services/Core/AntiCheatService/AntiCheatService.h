#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Журнал нарушений игроков. Системы-детекторы (например, PlayerAnimationSystem)
// фиксируют сюда подозрительные события через record(). Сам сервис ничего не
// предпринимает — решение, что делать с игроком (кик, бан, предупреждение),
// примет будущая система-античит, читая эти записи. Так детекторы и реакция
// разделены: добавить новый детектор или поменять политику реакции можно
// независимо друг от друга.
class AntiCheatService final : public IService
{
  public:
    enum class ViolationType : std::uint8_t
    {
        ForcedAnimationEscape, // игрок вышел из непрерываемой серверной анимации
        HealthHack,            // несанкционированный рост HP/брони (god mode / health hack)
        DamageHack,            // неправдоподобный give-damage (фейковый урон по другим)
        DeathEvasion,          // отказ умирать: игнор setHealth(0) или игра после серверной смерти
        TeleportHack,          // непровдоподобный скачок позиции / игнор серверного телепорта
        SpeedHack,             // устойчивое превышение физически возможной скорости
        StateHack,             // нелегальный переход стейта (мгновенный вход в ТС и т.п.)
        SpecialActionHack,     // джетпак без выдачи / побег из принудительного экшена
        WeaponHack,            // оружие без выдачи / стрельба без патронов
        ShotHack,              // фейковые данные пули: NaN, origin вдали от стрелка, за дальностью оружия
        RapidFire,             // темп стрельбы устойчиво выше возможного для оружия
        SilentAim,             // попадание по игроку без наведения камеры на него
        VehicleHack,           // repair hack / фейковый unoccupied-trailer sync
        PickupHack,            // подбор пикапа с неправдоподобной дистанции / из чужого мира
        CheckpointHack,        // вход в чекпоинт с неправдоподобной дистанции
        SpawnHack,             // запрос класса/спавна вне легального контекста (телепорт+хил респауном)
        CarShot,               // попадание из транспорта оружием, которым drive-by невозможен
        QuickTurn,             // серия мгновенных разворотов модели (CLEO quick turn)
        Count,                 // размер таблицы весов, не нарушение
    };

    struct Violation
    {
        ViolationType type;
        TimePoint time;
        std::string detail; // детали для лога (что требовалось / что заявил клиент)
    };

    struct PlayerRecord
    {
        std::uint32_t total = 0; // всего зафиксировано (не урезается лимитом recent)
        TimePoint firstAt;       // время первого нарушения
        TimePoint lastAt;        // время последнего нарушения
        std::vector<Violation> recent; // последние нарушения с деталями (ограничено)
        // Счёт сессии: сумма весов нарушений, помноженных на личный множитель
        // игрока. Достиг порога — система-реакция отключает игрока.
        float score = 0.0f;
        // Личный множитель (персист в БД, грузится на старте сессии): у обычного
        // игрока 1.0, у ранее пойманного выше — его терпимость меньше.
        float multiplier = 1.0f;
    };

    // Наблюдатель вызывается синхронно после каждой записи — так система-античит
    // реагирует на нарушения без поллинга журнала.
    using Observer = std::function<void(int playerId, ViolationType type, const PlayerRecord &record)>;

    // Имя типа для логов и дев-вывода. Единственное место соответствия «тип -> имя»:
    // локальные копии в системах отставали от enum и показывали Unknown.
    static const char *name(ViolationType type);

    // Зафиксировать нарушение. Вызывают системы-детекторы. Начисляет счёт:
    // score += weight(type) * multiplier(playerId).
    void record(int playerId, ViolationType type, std::string detail, TimePoint now);

    // --- балльная модель (настраивается дев-панелью в рантайме) ---
    // Вес одного нарушения в долях порога: 0.25 значит «четыре таких = порог».
    float weight(ViolationType type) const;
    void setWeight(ViolationType type, float value);
    // Порог отключения. Счёт достиг его — система-реакция кикает.
    float threshold() const;
    void setThreshold(float value);
    // На сколько выставляется личный множитель пойманного (персист в БД).
    float kickedMultiplier() const;
    void setKickedMultiplier(float value);

    // Агрессивный режим: пока на сервере некому смотреть за игроками вживую, счёт
    // начисляется быстрее — нарушитель добирает порог сам, без разбора админом.
    float noAdminMultiplier() const;
    void setNoAdminMultiplier(float value);
    // Предикат «админ онлайн» ставит система-реакция: сервис про админов не знает.
    // Пока предикат не задан, режим считается неактивным.
    using AdminPresenceCheck = std::function<bool()>;
    void setAdminPresenceCheck(AdminPresenceCheck check);
    bool aggressive() const; // сейчас ли действует надбавка

    // Предикат «это бот (NPC)». Боты двигаются серверными командами — рывками,
    // мимо клиентской физики, — и штатно выглядят для детекторов как телепорт или
    // спидхак. Ловить их бессмысленно: клиента, который мог бы читерить, там нет.
    using BotCheck = std::function<bool(int playerId)>;
    void setBotCheck(BotCheck check);

    // --- счёт и множитель игрока ---
    float score(int playerId) const;
    void resetScore(int playerId);
    float multiplier(int playerId) const;
    void setMultiplier(int playerId, float value); // грузится из БД на старте сессии

    // Подписка на нарушения (вызывается из конструкторов систем).
    void subscribe(Observer observer);

    // Чтение и сброс.
    const PlayerRecord &get(int playerId) const;
    std::uint32_t count(int playerId) const;
    bool flagged(int playerId) const;
    void clear(int playerId);

    // Дефолты весов/порога — как в коде, для кнопки «сбросить настройки» в панели.
    void resetTuning();

  private:
    static constexpr std::size_t RECENT_LIMIT = 20;
    static constexpr std::size_t TYPE_COUNT = static_cast<std::size_t>(ViolationType::Count);

    std::array<PlayerRecord, MAX_PLAYERS> m_records;
    std::vector<Observer> m_observers;

    // Настройки живут в памяти: их калибруют в игре дев-панелью, а обкатанные
    // значения переносят в дефолты кода. В БД не персистятся сознательно —
    // иначе боевая конфигурация анти-чита оказывается вне ревью.
    std::array<float, TYPE_COUNT> m_weights{};
    float m_threshold = 1.0f;
    float m_kickedMultiplier = 1.2f;
    float m_noAdminMultiplier = 1.2f;
    AdminPresenceCheck m_adminPresence;
    BotCheck m_botCheck;
};
