#include "Services/WeaponProficiencyService/WeaponProficiencyService.h"

#include <algorithm>
#include <utility>

WeaponProficiencyService::WeaponProficiencyService()
{
    // Все слоты — нули (value-init массивов в PlayerProficiency уже это делает;
    // fill оставлен явно как страховка от смены инициализации структуры).
    for (PlayerProficiency &p : m_state)
    {
        p.skill.fill(0);
        p.shotRemainder.fill(0);
    }
}

int WeaponProficiencyService::weaponIndex(std::uint8_t weaponId)
{
    // O(1) switch — hot path. Индексы стабильны: завязаны на колонку `weapon` в
    // БД через сопоставление в системе, само значение weapon хранится как id.
    switch (weaponId)
    {
    case WEAPON_DEAGLE:
        return 0;
    case WEAPON_SHOTGUN:
        return 1;
    case WEAPON_AK47:
        return 2;
    case WEAPON_M4:
        return 3;
    case WEAPON_SNIPER:
        return 4;
    default:
        return -1;
    }
}

bool WeaponProficiencyService::isTrackedWeapon(std::uint8_t weaponId)
{
    return weaponIndex(weaponId) >= 0;
}

void WeaponProficiencyService::registerShot(int playerId, std::uint8_t weaponId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    const int idx = weaponIndex(weaponId);
    if (idx < 0)
        return; // оружие не из 5 учитываемых — не считаем

    PlayerProficiency &p = m_state[playerId];
    if (p.skill[idx] >= MAX_SKILL)
        return; // кап достигнут — не копим остаток дальше, чтобы не крутить впустую

    // Порог пер-оружие; idx уже в [0; WEAPON_COUNT) (проверен выше), таблица того
    // же размера WEAPON_COUNT — индексация без OOB.
    if (++p.shotRemainder[idx] >= SHOTS_PER_SKILL[idx])
    {
        p.shotRemainder[idx] = 0;
        ++p.skill[idx]; // не превысит MAX_SKILL: выше отсекли уже-капнутый случай

        // Level-up: уведомляем наблюдателей (система применит нативный скилл).
        // Обход только на +1 (раз в SHOTS_PER_SKILL[idx] выстрелов) — обычная пуля
        // сюда не заходит, hot path остаётся O(1). weaponId уже учтённый (idx>=0).
        const int newSkill = p.skill[idx];
        for (const LevelUpObserver &observer : m_levelUpObservers)
            observer(playerId, weaponId, newSkill);
    }
}

int WeaponProficiencyService::getSkill(int playerId, std::uint8_t weaponId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    const int idx = weaponIndex(weaponId);
    if (idx < 0)
        return 0;
    return m_state[playerId].skill[idx];
}

void WeaponProficiencyService::setSkill(int playerId, std::uint8_t weaponId, int value)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    const int idx = weaponIndex(weaponId);
    if (idx < 0)
        return;

    // Клампим: мусор/устаревший кап из БД не должен осесть как валидный скилл.
    PlayerProficiency &p = m_state[playerId];
    p.skill[idx] = std::clamp(value, 0, MAX_SKILL);
    p.shotRemainder[idx] = 0; // остаток не персистится — на загрузке всегда 0
}

void WeaponProficiencyService::subscribeLevelUp(LevelUpObserver observer)
{
    m_levelUpObservers.push_back(std::move(observer));
}

void WeaponProficiencyService::resetPlayer(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    PlayerProficiency &p = m_state[playerId];
    p.skill.fill(0);
    p.shotRemainder.fill(0);
}
