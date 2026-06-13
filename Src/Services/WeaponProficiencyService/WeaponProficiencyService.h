#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

class WeaponProficiencySystem;

// Источник правды о ВЛАДЕНИИ оружием (кастомный стат прогрессии аккаунта) на
// время сессии. Это БИЗНЕС-стат аккаунта (как личный скин), НЕ нативный
// SA-MP weapon skill из WeaponSkillService (тот — 0..999, разброс/отдача/
// дуал-вилд). Здесь — отдельный «скилл владения» 0..100, который НАРАБАТЫВАЕТСЯ
// стрельбой и персистится в свою таблицу `player_weapon_skill`.
//
// Учитываются ровно 5 оружий (диглы/дробовик/AK-47/M4/снайперка); прочие — мимо.
// Прогрессия: каждые SHOTS_PER_SKILL валидных выстрелов из оружия → +1 скилл,
// кап MAX_SKILL. Остаток выстрелов (0..SHOTS_PER_SKILL-1) копится в памяти и НЕ
// персистится (потеря недобора на логауте допустима — так договорено).
//
// Чистый контейнер состояния (как PlayerPersonalSkinService): загрузка/запись в
// БД и привязка к сессии — на WeaponProficiencySystem; хук валидного выстрела —
// PlayerWeaponSystem (зовёт registerShot только при серверно-валидном выстреле).
// Сервис ни от кого не зависит (регистр конструирует его без зависимостей).
//
// Анти-чит: сервис сам по себе НЕ судит о валидности выстрела — он лишь считает.
// Вызывающий (PlayerWeaponSystem) обязан звать registerShot ТОЛЬКО на выстреле,
// прошедшем серверную валидацию (!outcome.drop): фейковые/rapid-fire/ammo-hack
// выстрелы дропаются валидатором и сюда не доходят — фарма скилла фейками нет.
class WeaponProficiencyService final : public IService
{
    friend WeaponProficiencySystem;

  public:
    // Скилл всегда в [0; MAX_SKILL]. Каждые SHOTS_PER_SKILL выстрелов → +1.
    static constexpr int MAX_SKILL = 100;
    static constexpr int SHOTS_PER_SKILL = 5;

    // Все слоты инициализируются нулём в конструкторе (массивы фиксированной
    // длины, без «не сидировано»: дефолт скилла и есть 0 — то же, что отсутствие
    // строки в БД, поэтому отдельный маркер не нужен).
    WeaponProficiencyService();

    // Учтённые оружия (серверные weapon id).
    static constexpr std::uint8_t WEAPON_DEAGLE = 24;
    static constexpr std::uint8_t WEAPON_SHOTGUN = 25;
    static constexpr std::uint8_t WEAPON_AK47 = 30;
    static constexpr std::uint8_t WEAPON_M4 = 31;
    static constexpr std::uint8_t WEAPON_SNIPER = 34;

    // Кол-во учитываемых оружий (= размер per-player массивов).
    static constexpr std::size_t WEAPON_COUNT = 5;

    // true — оружие учитывается прогрессией (есть в таблице из 5).
    static bool isTrackedWeapon(std::uint8_t weaponId);

    // HOT PATH (каждый валидный выстрел). Строго O(1), без аллокаций, без БД.
    // weaponId не из 5 учитываемых — тихо игнорируется. Каждый SHOTS_PER_SKILL-й
    // выстрел из учтённого оружия даёт +1 скилл (кап MAX_SKILL), счётчик-остаток
    // обнуляется. Вызывать ТОЛЬКО на серверно-валидном выстреле (см. шапку).
    void registerShot(int playerId, std::uint8_t weaponId);

    // Текущий скилл владения [0; MAX_SKILL]. Невалидный playerId/оружие → 0.
    int getSkill(int playerId, std::uint8_t weaponId) const;

    // Загрузка из БД: пишет скилл, КЛАМПИТ в [0; MAX_SKILL] (мусор/устаревший кап
    // из БД не осядет). Невалидный playerId/оружие — игнор. Остаток-счётчик при
    // загрузке обнуляется (он не персистится).
    void setSkill(int playerId, std::uint8_t weaponId, int value);

    // Наблюдатель level-up: зовётся СИНХРОННО из registerShot ровно тогда, когда
    // владение оружием поднялось на +1 (раз в SHOTS_PER_SKILL валидных выстрелов).
    // newSkill — уже обновлённое значение [1; MAX_SKILL]. Через этот сигнал
    // WeaponProficiencySystem применяет нативный weapon skill, НЕ заставляя сервис
    // зависеть от WeaponSkillService (источников правды по-прежнему два разных
    // стата, связка — в системе). Аналог FactionService::subscribeMemberChange.
    using LevelUpObserver = std::function<void(int playerId, std::uint8_t weaponId, int newSkill)>;
    void subscribeLevelUp(LevelUpObserver observer);

  private:
    // Вызывается WeaponProficiencySystem на коннекте/дисконнекте.
    void resetPlayer(int playerId);

    // weaponId → индекс [0; WEAPON_COUNT) или -1, если оружие не учитывается.
    // O(1) switch — это hot path (registerShot на каждой пуле).
    static int weaponIndex(std::uint8_t weaponId);

    struct PlayerProficiency
    {
        // skill[i] — скилл владения i-м оружием [0; MAX_SKILL].
        // shotRemainder[i] — выстрелы сверх последнего +1 (0..SHOTS_PER_SKILL-1).
        std::array<int, WEAPON_COUNT> skill{};         // value-init → все 0
        std::array<int, WEAPON_COUNT> shotRemainder{}; // value-init → все 0
    };

    std::array<PlayerProficiency, MAX_PLAYERS> m_state;

    // Подписчики на level-up. Обходятся синхронно в registerShot при каждом +1.
    // Подписка — на старте (конструктор системы), в hot path только обход вектора
    // (обычно один элемент). Уведомление происходит лишь раз в SHOTS_PER_SKILL
    // выстрелов, поэтому обычная пуля наблюдателей не трогает (O(1) без обхода).
    std::vector<LevelUpObserver> m_levelUpObservers;
};
