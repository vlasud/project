#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"

#include <chrono>
#include <cmath>
#include <fmt/format.h>
#include <glm/geometric.hpp>

namespace
{
// Грейс после серверной выдачи/изъятия: клиенту нужно применить RPC.
constexpr std::chrono::milliseconds SYNC_GRACE{1500};

// Не чаще этого фиксируем повторное нарушение по одному игроку (чит, который
// игнорирует снятие оружия, и так наберёт порог кика — без спама в журнал).
constexpr std::chrono::milliseconds FLAG_COOLDOWN{2000};

// Допустимый «долг» патронов: дрейф учёта на лагах и drive-by (его выстрелы не
// приходят bullet sync'ом). Стрельба глубже долга — ammo hack.
constexpr std::int32_t AMMO_DEBT = 10;

bool rateLimited(TimePoint &lastFlag, TimePoint now)
{
    if (now - lastFlag < FLAG_COOLDOWN)
        return true;
    lastFlag = now;
    return false;
}

// --- валидация выстрела -----------------------------------------------------

// Запас дальности: цель на сервере успела уехать от места, где её видел клиент,
// плюс позиция машины — её центр, а корпус длинный (поезд, Andromada).
constexpr float RANGE_SLACK = 30.0f;
// Окно лага для допусков, масштабируемых скоростью цели (пинг + интерполяция).
constexpr float LAG_WINDOW = 0.6f;
// Origin пули не дальше этого от принятой позиции стрелка (дуло машины на ходу,
// рассинхрон последнего sync — всё в пределах; spoof обычно за сотни метров).
constexpr float ORIGIN_SLACK_SQ = 15.0f * 15.0f;
// Кредит leaky bucket: на сколько суммарно темп может опережать таблицу,
// прежде чем это перестаёт быть сгустком пакетов и становится rapid fire.
constexpr std::chrono::milliseconds ROF_BURST{1000};
// Silent aim: базовый промах луча камеры мимо цели + рост с дистанцией.
constexpr float AIM_BASE = 4.0f;
constexpr float AIM_PER_METER = 0.05f;
constexpr float AIM_MIN_DISTANCE = 3.0f; // в упор углы не показательны

struct WeaponShotSpec
{
    float range;                           // дальность из weapon.dat
    std::chrono::milliseconds minInterval; // быстрее самого быстрого легального
                                           // (dual-wield, c-bug) в ~2 раза
};

WeaponShotSpec shotSpec(std::uint8_t weaponId)
{
    switch (weaponId)
    {
    case 22:
        return {35.0f, std::chrono::milliseconds(80)}; // Colt45 (dual)
    case 23:
        return {35.0f, std::chrono::milliseconds(150)}; // Silenced
    case 24:
        return {35.0f, std::chrono::milliseconds(200)}; // Deagle (c-bug)
    case 25:
        return {40.0f, std::chrono::milliseconds(200)}; // Shotgun (c-bug)
    case 26:
        return {35.0f, std::chrono::milliseconds(80)}; // Sawnoff (dual)
    case 27:
        return {40.0f, std::chrono::milliseconds(150)}; // Spas-12
    case 28:
        return {35.0f, std::chrono::milliseconds(40)}; // Uzi (dual)
    case 29:
        return {45.0f, std::chrono::milliseconds(40)}; // MP5
    case 30:
        return {70.0f, std::chrono::milliseconds(40)}; // AK-47
    case 31:
        return {90.0f, std::chrono::milliseconds(40)}; // M4
    case 32:
        return {35.0f, std::chrono::milliseconds(40)}; // Tec-9 (dual)
    case 33:
        return {100.0f, std::chrono::milliseconds(300)}; // Rifle (c-bug)
    case 34:
        return {320.0f, std::chrono::milliseconds(300)}; // Sniper (c-bug)
    case 38:
        return {75.0f, std::chrono::milliseconds(10)}; // Minigun
    default:
        return {320.0f, std::chrono::milliseconds(40)}; // незнакомое — щедро
    }
}

bool finite(const Vector3 &v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
} // namespace

Milliseconds PlayerWeaponService::minShotInterval(std::uint8_t weaponId)
{
    return shotSpec(weaponId).minInterval;
}

PlayerWeaponService::Slot *PlayerWeaponService::findWeapon(State &st, std::uint8_t weaponId)
{
    const std::uint8_t slot = WeaponSlotData(weaponId).slot();
    if (slot == INVALID_WEAPON_SLOT)
        return nullptr;
    Slot &entry = st.slots[slot];
    return entry.id == weaponId ? &entry : nullptr;
}

const PlayerWeaponService::Slot *PlayerWeaponService::findWeapon(const State &st, std::uint8_t weaponId) const
{
    const std::uint8_t slot = WeaponSlotData(weaponId).slot();
    if (slot == INVALID_WEAPON_SLOT)
        return nullptr;
    const Slot &entry = st.slots[slot];
    return entry.id == weaponId ? &entry : nullptr;
}

bool PlayerWeaponService::hasWeapon(int playerId, std::uint8_t weaponId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    return findWeapon(m_state[playerId], weaponId) != nullptr;
}

int PlayerWeaponService::getAmmo(int playerId, std::uint8_t weaponId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return -1;
    const Slot *slot = findWeapon(m_state[playerId], weaponId);
    if (!slot)
        return -1;
    return slot->ammo > 0 ? slot->ammo : 0;
}

std::uint8_t PlayerWeaponService::getArmedWeapon(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    return m_state[playerId].armed;
}

void PlayerWeaponService::giveWeapon(IPlayer &player, std::uint8_t weaponId, std::uint32_t ammo)
{
    const std::uint8_t slotIndex = WeaponSlotData(weaponId).slot();
    if (slotIndex == INVALID_WEAPON_SLOT)
        return;

    State &st = m_state[player.getID()];
    Slot &slot = st.slots[slotIndex];
    if (slot.id == weaponId)
    {
        slot.ammo += static_cast<std::int32_t>(ammo); // как в GTA: патроны складываются
        if (slot.ammo < 0)
            slot.ammo = 0;
    }
    else
    {
        slot = {weaponId, static_cast<std::int32_t>(ammo)}; // оружие слота заменяется
    }
    st.lastChange = std::chrono::steady_clock::now();

    player.giveWeapon(WeaponSlotData{weaponId, static_cast<std::uint32_t>(slot.ammo)});
}

void PlayerWeaponService::removeWeapon(IPlayer &player, std::uint8_t weaponId)
{
    State &st = m_state[player.getID()];
    if (Slot *slot = findWeapon(st, weaponId))
    {
        *slot = {};
        st.lastChange = std::chrono::steady_clock::now();
    }
    player.removeWeapon(weaponId);
}

void PlayerWeaponService::resetWeapons(IPlayer &player)
{
    State &st = m_state[player.getID()];
    st.slots = {};
    st.armed = 0;
    st.lastChange = std::chrono::steady_clock::now();
    player.resetWeapons();
}

void PlayerWeaponService::setAmmo(IPlayer &player, std::uint8_t weaponId, std::uint32_t ammo)
{
    State &st = m_state[player.getID()];
    if (Slot *slot = findWeapon(st, weaponId))
    {
        slot->ammo = static_cast<std::int32_t>(ammo);
        st.lastChange = std::chrono::steady_clock::now();
        player.setWeaponAmmo(WeaponSlotData{weaponId, ammo});
    }
}

PlayerWeaponService::ShotOutcome PlayerWeaponService::onShot(IPlayer &player, const PlayerBulletData &bullet,
                                                             const ShotContext &ctx, TimePoint now)
{
    ShotOutcome outcome;
    State &st = m_state[player.getID()];
    const std::uint8_t weaponId = bullet.weapon;

    // Невалидный выстрел отбрасываем ВСЕГДА; в журнал пишем с рейт-лимитом.
    auto flag = [&](ShotFlag kind, std::string detail)
    {
        outcome.drop = true;
        if (!rateLimited(st.lastFlag, now))
        {
            outcome.flag = kind;
            outcome.detail = std::move(detail);
        }
    };

    // NaN/Inf: проверки границ ядра NaN проходит (NaN > b == false), а дальше
    // эти числа разойдутся по всем потребителям событий выстрела.
    if (!finite(bullet.origin) || !finite(bullet.hitPos) || !finite(bullet.offset))
    {
        flag(ShotFlag::ShotHack, fmt::format("non-finite bullet data, weapon {}", weaponId));
        return outcome;
    }

    Slot *slot = findWeapon(st, weaponId);
    if (!slot)
    {
        // Стрельба из оружия, которого сервер не выдавал.
        outcome.drop = true;
        if (!rateLimited(st.lastFlag, now))
        {
            player.removeWeapon(weaponId);
            player.setArmedWeapon(0);
            outcome.flag = ShotFlag::WeaponHack;
            outcome.detail = fmt::format("shot with unowned weapon {}", weaponId);
        }
        return outcome;
    }

    --slot->ammo;
    if (slot->ammo < -AMMO_DEBT)
    {
        // Стрельба при серверном нуле патронов глубже допуска на дрейф.
        slot->ammo = 0;
        player.setWeaponAmmo(WeaponSlotData{weaponId, 0});
        flag(ShotFlag::WeaponHack, fmt::format("shooting weapon {} with no ammo", weaponId));
        return outcome;
    }

    const WeaponShotSpec spec = shotSpec(weaponId);

    // Темп: каждый выстрел кладёт в bucket интервал своего оружия, время его
    // осушает. Разовый сгусток пакетов после лаг-спайка съест кредит и
    // восстановится; читовый темп держит bucket у потолка — всё дропается.
    if (st.rofBucket < now - ROF_BURST)
        st.rofBucket = now - ROF_BURST;
    st.rofBucket += spec.minInterval;
    if (st.rofBucket > now + ROF_BURST)
    {
        st.rofBucket = now + ROF_BURST;
        flag(ShotFlag::RapidFire,
             fmt::format("rapid fire weapon {}: sustained rate above 1/{}ms", weaponId, spec.minInterval.count()));
        return outcome;
    }

    // Origin рядом со стрелком: silent aim часто рисует выстрел «из» цели.
    const Vector3 originDelta = bullet.origin - ctx.shooterPos;
    if (glm::dot(originDelta, originDelta) > ORIGIN_SLACK_SQ)
    {
        flag(ShotFlag::ShotHack,
             fmt::format("bullet origin {:.0f}m away from shooter, weapon {}", glm::length(originDelta), weaponId));
        return outcome;
    }

    if (ctx.targetPos)
    {
        // Дальность до СЕРВЕРНОЙ позиции цели (hitPos рисует клиент — не факт).
        const float targetDistance = glm::length(*ctx.targetPos - bullet.origin);
        if (targetDistance > spec.range + RANGE_SLACK + ctx.targetSpeed * LAG_WINDOW)
        {
            flag(ShotFlag::ShotHack,
                 fmt::format("hit at {:.0f}m exceeds weapon {} range {:.0f}m", targetDistance, weaponId, spec.range));
            return outcome;
        }

        // Silent aim: луч камеры обязан проходить рядом с целью. Допуск растёт
        // с дистанцией (разброс/упреждение) и скоростью цели (лаг).
        if (ctx.checkSilentAim)
        {
            const PlayerAimData &aim = player.getAimData();
            const Vector3 toTarget = *ctx.targetPos - aim.camPos;
            const float distance = glm::length(toTarget);
            const float frontLen = glm::length(aim.camFrontVector);
            if (finite(aim.camFrontVector) && finite(aim.camPos) && frontLen > 0.1f && distance > AIM_MIN_DISTANCE)
            {
                const float along = glm::dot(aim.camFrontVector / frontLen, toTarget);
                const float threshold = AIM_BASE + distance * AIM_PER_METER + ctx.targetSpeed * LAG_WINDOW;
                const float offAxisSq = glm::dot(toTarget, toTarget) - along * along;
                if (along < 0.0f || offAxisSq > threshold * threshold)
                {
                    flag(ShotFlag::SilentAim,
                         fmt::format("hit player {:.0f}m off aim ray at {:.0f}m, weapon {}",
                                     along < 0.0f ? distance : std::sqrt(offAxisSq), distance, weaponId));
                    return outcome;
                }
            }
        }
    }
    return outcome;
}

PlayerWeaponService::Outcome PlayerWeaponService::verifyArmed(IPlayer &player, TimePoint now)
{
    Outcome outcome;
    State &st = m_state[player.getID()];

    const std::uint8_t reported = static_cast<std::uint8_t>(player.getArmedWeapon());

    // Кулаки — всегда; парашют игра выдаёт сама при прыжке из самолёта;
    // детонатор появляется вместе с выданными сатчелами.
    if (reported == 0 || reported == 46 || (reported == 40 && findWeapon(st, 39)))
    {
        st.armed = reported;
        return outcome;
    }

    if (findWeapon(st, reported))
    {
        st.armed = reported;
        return outcome;
    }

    // В руках оружие, которого нет в серверном инвентаре.
    if (now - st.lastChange < SYNC_GRACE)
        return outcome; // клиент ещё применяет недавнюю выдачу/изъятие

    if (!rateLimited(st.lastFlag, now))
    {
        player.removeWeapon(reported);
        player.setArmedWeapon(0);
        outcome.weaponHack = true;
        outcome.detail = fmt::format("armed unowned weapon {}", reported);
    }
    return outcome;
}

void PlayerWeaponService::onSpawn(IPlayer &player)
{
    // GTA теряет оружие на смерти — серверный инвентарь чистится, игровая логика
    // перевыдаёт через сервис.
    State &st = m_state[player.getID()];
    st.slots = {};
    st.armed = 0;
    st.lastChange = std::chrono::steady_clock::now();
}

void PlayerWeaponService::reset(int playerId)
{
    m_state[playerId] = State{};
}
