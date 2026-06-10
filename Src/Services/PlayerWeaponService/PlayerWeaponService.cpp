#include "PlayerWeaponService.h"

#include <chrono>
#include <fmt/format.h>

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
} // namespace

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

PlayerWeaponService::Outcome PlayerWeaponService::onShot(IPlayer &player, std::uint8_t weaponId, TimePoint now)
{
    Outcome outcome;
    State &st = m_state[player.getID()];

    Slot *slot = findWeapon(st, weaponId);
    if (!slot)
    {
        // Стрельба из оружия, которого сервер не выдавал.
        if (!rateLimited(st.lastFlag, now))
        {
            player.removeWeapon(weaponId);
            player.setArmedWeapon(0);
            outcome.weaponHack = true;
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
        if (!rateLimited(st.lastFlag, now))
        {
            outcome.weaponHack = true;
            outcome.detail = fmt::format("shooting weapon {} with no ammo", weaponId);
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
