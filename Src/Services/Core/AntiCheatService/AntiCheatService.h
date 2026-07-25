#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
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
    };

    // Наблюдатель вызывается синхронно после каждой записи — так система-античит
    // реагирует на нарушения без поллинга журнала.
    using Observer = std::function<void(int playerId, ViolationType type, const PlayerRecord &record)>;

    // Имя типа для логов и дев-вывода. Единственное место соответствия «тип -> имя»:
    // локальные копии в системах отставали от enum и показывали Unknown.
    static const char *name(ViolationType type);

    // Зафиксировать нарушение. Вызывают системы-детекторы.
    void record(int playerId, ViolationType type, std::string detail, TimePoint now);

    // Подписка на нарушения (вызывается из конструкторов систем).
    void subscribe(Observer observer);

    // Чтение и сброс.
    const PlayerRecord &get(int playerId) const;
    std::uint32_t count(int playerId) const;
    bool flagged(int playerId) const;
    void clear(int playerId);

  private:
    static constexpr std::size_t RECENT_LIMIT = 20;

    std::array<PlayerRecord, MAX_PLAYERS> m_records;
    std::vector<Observer> m_observers;
};
