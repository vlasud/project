#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <string>

// Сервис оружия — серверный инвентарь как единственный источник истины о том,
// какое оружие и сколько патронов есть у игрока.
//
// Клиент может «нарисовать» себе любое оружие локально (weapon hack) — оружие в
// руках приходит в каждом sync. Поэтому:
//  * выдача/изъятие — ТОЛЬКО через giveWeapon/removeWeapon этого сервиса;
//  * на каждом апдейте оружие в руках клиента сверяется с инвентарём: чужое —
//    снимается + нарушение;
//  * каждый выстрел (bullet sync) проверяется на владение и списывает патрон;
//    стрельба при серверном нуле патронов (с запасом на дрейф) — ammo hack:
//    патроны обнуляются принудительно + нарушение.
//
// Валидация самого выстрела (ядро omp уже отсекло нестреляющее оружие,
// несуществующую/незастримленную цель и выстрел в себя):
//  * NaN/Inf в данных пули — проверки границ ядра NaN проходит (NaN > b == false);
//  * origin рядом с принятой позицией стрелка (origin spoof);
//  * дальность до СЕРВЕРНОЙ позиции цели в пределах дальности оружия;
//  * темп стрельбы — leaky bucket: устойчивое превышение скорострельности
//    оружия (разовые сгустки пакетов после лаг-спайка прощаются);
//  * silent aim — попадание по игроку, на которого камера не наведена
//    (только пешком: авто-прицел драйв-бая легально стреляет под углом).
// Все пороги щедрые (лаг, c-bug, dual-wield): читы превышают их в разы.
// Невалидный выстрел отбрасывается (drop) — урон по нему не регистрируется.
//
// На спавне инвентарь чистится (GTA теряет оружие на смерти) — игровая логика
// перевыдаёт через сервис.
class PlayerWeaponService final : public IService
{
  public:
    // Табличный минимальный интервал между выстрелами (анти-rapid-fire) — для
    // тулз настройки темпа.
    static Milliseconds minShotInterval(std::uint8_t weaponId);

    // --- источник истины ---
    bool hasWeapon(int playerId, std::uint8_t weaponId) const;
    int getAmmo(int playerId, std::uint8_t weaponId) const; // -1 — оружия нет
    std::uint8_t getArmedWeapon(int playerId) const;        // принятое оружие в руках

    // --- серверные операции ---
    void giveWeapon(IPlayer &player, std::uint8_t weaponId, std::uint32_t ammo);
    void removeWeapon(IPlayer &player, std::uint8_t weaponId);
    void resetWeapons(IPlayer &player);
    void setAmmo(IPlayer &player, std::uint8_t weaponId, std::uint32_t ammo);

    struct Outcome
    {
        bool weaponHack = false;
        std::string detail;
    };

    enum class ShotFlag : std::uint8_t
    {
        None,
        WeaponHack, // оружие без выдачи / без патронов
        ShotHack,   // фейковые данные пули (NaN, origin spoof, за дальностью)
        RapidFire,
        SilentAim,
    };

    struct ShotOutcome
    {
        ShotFlag flag = ShotFlag::None;
        bool drop = false; // выстрел не пускать дальше по конвейеру (урон не регистрировать)
        std::string detail;
    };

    // Серверные факты для сверки — собирает PlayerWeaponSystem.
    struct ShotContext
    {
        Vector3 shooterPos{};               // принятая позиция стрелка (LocationService)
        const Vector3 *targetPos = nullptr; // серверная позиция цели (null — промах)
        float targetSpeed = 0.0f;           // м/с — лаговый допуск дальности и прицела
        bool checkSilentAim = false;        // цель — игрок и стрелок пешком
    };

    // --- вызывается PlayerWeaponSystem ---
    ShotOutcome onShot(IPlayer &player, const PlayerBulletData &bullet, const ShotContext &ctx,
                       TimePoint now);                   // из bullet sync
    Outcome verifyArmed(IPlayer &player, TimePoint now); // каждый апдейт
    void onSpawn(IPlayer &player);
    void reset(int playerId);

  private:
    struct Slot
    {
        std::uint8_t id = 0;
        std::int32_t ammo = 0; // signed: уходит в минус до порога долга (дрейф)
    };

    struct State
    {
        std::array<Slot, MAX_WEAPON_SLOTS> slots;
        std::uint8_t armed = 0; // принятое оружие в руках
        TimePoint lastChange;   // грейс синхронизации после выдачи/изъятия
        TimePoint lastFlag;     // rate limit повторных нарушений
        TimePoint rofBucket;    // leaky bucket темпа стрельбы (каждый выстрел += интервал оружия)
    };

    Slot *findWeapon(State &st, std::uint8_t weaponId);
    const Slot *findWeapon(const State &st, std::uint8_t weaponId) const;

    std::array<State, MAX_PLAYERS> m_state;
};
