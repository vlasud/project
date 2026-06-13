#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "values.hpp"
#include <array>
#include <cstdint>

class WeaponSkillSystem;

// Единственный источник правды об уровнях владения оружием (weapon skill levels).
//
//   m_skills.setLevel(player, PlayerWeaponSkill_Sniper, 999); // выше отдача/прицел
//   m_skills.maxOut(player);                                  // все навыки в максимум
//   m_skills.getLevel(playerId, PlayerWeaponSkill_M4);        // серверная правда
//
// Скилл-левелы в SA-MP СЕРВЕРНО-АВТОРИТЕТНЫ: рост от использования на сервере
// отключён, отдельного клиентского RPC/синка для скиллов нет — клиент сам их не
// повышает. Поэтому валидировать тут нечего на стороне сети; абьюз-поверхность —
// только некорректные аргументы в наш API и обход источника правды. Никто не
// зовёт player.setSkillLevel() напрямую — иначе источников правды станет два
// (наш массив и состояние клиента разойдутся, переприменение на спавне молча
// откатит «чужое» значение).
//
// Скилл влияет на разброс, перезарядку, отдачу и доступность двойного
// удержания (uzi/tec/sawn-off) — это чисто данные сервер -> клиент.
class WeaponSkillService final : public IService
{
    friend WeaponSkillSystem;

  public:
    // Кол-во валидных скиллов (PlayerWeaponSkill_Pistol..._Sniper). Совпадает с
    // NUM_SKILL_LEVELS из SDK — храним ровно столько, сколько принимает клиент.
    static constexpr int NUM_SKILLS = NUM_SKILL_LEVELS; // 11

    // skill в пределах 0..NUM_SKILLS-1 (PlayerWeaponSkill_Invalid и любой выход
    // за границы — невалиден: в массив не пишем, к клиенту не применяем).
    static bool isValidSkill(PlayerWeaponSkill skill);

    // Задать уровень одного навыка. level клампится в 0..MAX_SKILL_LEVEL.
    // Возвращает false при невалидном playerId или skill — состояние не тронуто.
    bool setLevel(IPlayer &player, PlayerWeaponSkill skill, int level);

    // Задать все навыки разом одним значением (клампится так же).
    bool setAllLevels(IPlayer &player, int level);

    // Удобные пресеты: все в максимум / все в ноль.
    bool maxOut(IPlayer &player);
    bool reset(IPlayer &player);

    // Серверная правда. Невалидный playerId/skill -> 0 (никогда не выходит за
    // границы массива).
    int getLevel(int playerId, PlayerWeaponSkill skill) const;

  private:
    static bool isValidPlayer(int playerId);
    static int clampLevel(int level);

    // Переприменить весь сохранённый набор к клиенту (вызывается на спавне).
    void reapply(IPlayer &player);

    // Сброс состояния игрока к нулям (вызывается WeaponSkillSystem).
    void resetPlayer(int playerId);

    // [playerId][skillIndex] — заданные сервером уровни. uint16_t: MAX_SKILL_LEVEL
    // (999) помещается, и тип совпадает с тем, что отдаёт SDK getSkillLevels().
    // По умолчанию все нули — стартовое «нулевое владение».
    std::array<std::array<std::uint16_t, NUM_SKILLS>, MAX_PLAYERS> m_levels{};
};
