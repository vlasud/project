#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Сервис оружия — серверный инвентарь как единственный источник истины о том,
// какое оружие и сколько патронов есть у игрока.
//
// Клиент может «нарисовать» себе любое оружие локально (weapon hack) — оружие в
// руках приходит в каждом sync. Поэтому:
//  * выдача/изъятие — ТОЛЬКО через giveWeapon/removeWeapon этого сервиса;
//  * на каждом апдейте оружие в руках клиента сверяется с инвентарём: чужое —
//    снимается + нарушение;
//  * там же сверяются патроны по слотам, монотонно (как HP в PlayerHealthService):
//    рост над серверным остатком запрещён — это ammo hack, патроны форсятся назад
//    + нарушение; снижение принимается за правду (drive-by bullet sync не шлёт,
//    пакеты теряются — иначе рассинхрон копится и прячет хак);
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
//  * silent aim — попадание по игроку, на которого камера не наведена: направление
//    камеры сверяется с серверной геометрией «принятая позиция стрелка → серверная
//    позиция цели с упреждением», позиция камеры из aim sync не используется
//    (её никто не валидирует). Отсутствие данных прицела вне грейса спавна — тоже
//    нарушение: иначе клиент обходил бы проверку, просто не присылая aim sync.
//    Вторая, независимая от aim sync опора — разворот корпуса стрелка: цель строго
//    за спиной легальной стрельбой не объясняется. Только пешком: авто-прицел
//    драйв-бая легально стреляет под углом.
// Все пороги щедрые (лаг, c-bug, dual-wield): читы превышают их в разы.
// Невалидный выстрел отбрасывается (drop) — урон по нему не регистрируется.
//
// На спавне инвентарь чистится (GTA теряет оружие на смерти) — игровая логика
// перевыдаёт через сервис.
class PlayerWeaponService final : public IService
{
  public:
    // Минимальный интервал между выстрелами (анти-rapid-fire): реальный shootTime
    // оружия из SDK, поделённый на запас (лаг, c-bug, dual-wield). ЕДИНЫЙ источник
    // темпа — им же ограничен темп хитов в PlayerHealthSystem, чтобы более щедрая
    // копия не обесценивала этот лимит. Открыт и для тулз замера темпа (/rof).
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

    // --- read-only снимок для персиста (см. PlayerWeaponPersistSystem) ---
    // Текущий набор непустых слотов: (weaponId, ammo). out очищается перед
    // заполнением; ammo клампится к >=0 (Slot::ammo может уйти в минус до порога
    // долга на ammo-hack — отрицательное сохранять бессмысленно). Bounds-safe.
    void getWeapons(int playerId, std::vector<std::pair<std::uint8_t, int>> &out) const;

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
        Vector3 targetVelocity{};           // м/с — упреждение позиции цели и лаговые допуски
        bool checkSilentAim = false;        // цель — игрок и стрелок пешком
    };

    // --- вызывается PlayerWeaponSystem ---
    ShotOutcome onShot(IPlayer &player, const PlayerBulletData &bullet, const ShotContext &ctx,
                       TimePoint now);                   // из bullet sync
    // Сверка заявленного клиентом состояния (оружие в руках + патроны по слотам)
    // с серверным инвентарём. Каждый апдейт.
    Outcome verifySync(IPlayer &player, TimePoint now);
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
        TimePoint spawnAt;      // грейс на первые данные прицела после спавна
    };

    Slot *findWeapon(State &st, std::uint8_t weaponId);
    const Slot *findWeapon(const State &st, std::uint8_t weaponId) const;

    // Части verifySync. verifyArmedWeapon возвращает false, если смотреть патроны
    // уже нет смысла: оружие в руках чужое либо идёт грейс синхронизации.
    bool verifyArmedWeapon(IPlayer &player, State &st, TimePoint now, Outcome &outcome);
    void verifyAmmo(IPlayer &player, State &st, TimePoint now, Outcome &outcome);

    std::array<State, MAX_PLAYERS> m_state;
};
