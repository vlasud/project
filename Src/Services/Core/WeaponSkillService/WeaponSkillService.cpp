#include "Services/Core/WeaponSkillService/WeaponSkillService.h"

bool WeaponSkillService::isValidSkill(PlayerWeaponSkill skill)
{
    // PlayerWeaponSkill_Invalid == -1 и любой выход за верхнюю границу
    // отсекаются здесь же — индекс в массив идёт только после этой проверки.
    return skill >= PlayerWeaponSkill_Pistol && static_cast<int>(skill) < NUM_SKILLS;
}

bool WeaponSkillService::isValidPlayer(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}

int WeaponSkillService::clampLevel(int level)
{
    // Отрицательный или выше потолка SA — к границам: клиент принимает только
    // 0..MAX_SKILL_LEVEL, выход за диапазон ведёт себя непредсказуемо.
    if (level < 0)
    {
        return 0;
    }
    if (level > MAX_SKILL_LEVEL)
    {
        return MAX_SKILL_LEVEL;
    }
    return level;
}

bool WeaponSkillService::setLevel(IPlayer &player, PlayerWeaponSkill skill, int level)
{
    const int playerId = player.getID();
    if (!isValidPlayer(playerId) || !isValidSkill(skill))
    {
        return false;
    }

    const int clamped = clampLevel(level);
    m_levels[playerId][static_cast<std::size_t>(skill)] = static_cast<std::uint16_t>(clamped);
    player.setSkillLevel(skill, clamped); // сразу, без ожидания респауна
    return true;
}

bool WeaponSkillService::setAllLevels(IPlayer &player, int level)
{
    const int playerId = player.getID();
    if (!isValidPlayer(playerId))
    {
        return false;
    }

    const int clamped = clampLevel(level);
    auto &levels = m_levels[playerId];
    for (int skill = 0; skill < NUM_SKILLS; ++skill)
    {
        levels[static_cast<std::size_t>(skill)] = static_cast<std::uint16_t>(clamped);
        player.setSkillLevel(static_cast<PlayerWeaponSkill>(skill), clamped);
    }
    return true;
}

bool WeaponSkillService::maxOut(IPlayer &player)
{
    return setAllLevels(player, MAX_SKILL_LEVEL);
}

bool WeaponSkillService::reset(IPlayer &player)
{
    return setAllLevels(player, 0);
}

int WeaponSkillService::getLevel(int playerId, PlayerWeaponSkill skill) const
{
    if (!isValidPlayer(playerId) || !isValidSkill(skill))
    {
        return 0;
    }
    return m_levels[playerId][static_cast<std::size_t>(skill)];
}

void WeaponSkillService::reapply(IPlayer &player)
{
    const int playerId = player.getID();
    if (!isValidPlayer(playerId))
    {
        return;
    }

    // Переприменяем весь набор как источник правды: делает состояние клиента
    // детерминированным после любого респауна, даже если что-то серверно сбило
    // навык. Согласовано с PlayerSkinService (тот тоже держит правду на сервере).
    const auto &levels = m_levels[playerId];
    for (int skill = 0; skill < NUM_SKILLS; ++skill)
    {
        player.setSkillLevel(static_cast<PlayerWeaponSkill>(skill), levels[static_cast<std::size_t>(skill)]);
    }
}

void WeaponSkillService::resetPlayer(int playerId)
{
    if (!isValidPlayer(playerId))
    {
        return;
    }
    m_levels[playerId].fill(0);
}
