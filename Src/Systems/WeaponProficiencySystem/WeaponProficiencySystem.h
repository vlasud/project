#pragma once

#include "Macro.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/WeaponSkillService/WeaponSkillService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/WeaponProficiencyService/WeaponProficiencyService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <cstdint>

// Проводник скилла владения оружием: лайфцикл слота и персист в БД.
//
//  * коннект/дисконнект — сброс слота WeaponProficiencyService к нулям (как
//    PlayerPersonalSkinSystem чистит свой слот), чтобы скилл одного аккаунта не
//    утёк в тот же playerId-слот следующему игроку;
//  * старт сессии (subscribeStart) — async-загрузка скиллов из
//    `player_weapon_skill` с serial-guard (как FactionSystem::loadMembership):
//    запрос на воркере вычитывает строки в владеющий вектор, на главном потоке
//    сверяется serial и значения раскладываются в сервис через setSkill (клампит);
//  * конец сессии (subscribeEnd) — UPSERT текущих скиллов всех 5 оружий в БД
//    одним throwQuery. Пишем синхронно на конце сессии (accountId ещё валиден),
//    НЕ на каждый выстрел: инкремент — hot path, в БД не лезет.
//
// Сам инкремент (registerShot) делает PlayerWeaponSystem на серверно-валидном
// выстреле — эта система прогрессию не двигает, только грузит/сохраняет.
//
// СВЯЗКА С НАТИВНЫМ SKILL. Бизнес-стат «владение» (0..100) проецируется на
// нативный SA-MP weapon skill (0..999, WeaponSkillService): чем выше владение,
// тем лучше реально держится оружие (разброс/отдача/перезарядка). Применяем
// нативный уровень в двух точках, обе — событийные (не hot path):
//   * на загрузке владения из БД (loadProficiency) — по всем 5 оружиям;
//   * на level-up (+1) — через WeaponProficiencyService::subscribeLevelUp, по
//     одному изменившемуся оружию (сервис не зависит от WeaponSkillService).
// Спавн отдельно НЕ хукаем: WeaponSkillService держит свой массив-источник
// правды и переприменяет его на спавне (WeaponSkillSystem::onPlayerSpawn), а мы
// пишем в него через setLevel — значит уровни переживают респавн сами.
class WeaponProficiencySystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    WeaponProficiencySystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    void loadProficiency(IPlayer &player, const PlayerSessionService::Session &session);
    void persistProficiency(IPlayer &player, const PlayerSessionService::Session &session);
    // /skills — показать игроку текущее владение по 5 учтённым оружиям.
    void showSkills(IPlayer &player);

    // Серверный weapon id (один из 5 учтённых) → нативная категория навыка.
    // Невалидное/неучтённое оружие → PlayerWeaponSkill_Invalid (применять нельзя).
    static PlayerWeaponSkill nativeSkillFor(std::uint8_t weaponId);
    // Владение 0..MAX_SKILL → нативный уровень 0..MAX_SKILL_LEVEL (линейно).
    static int toNativeLevel(int proficiency);
    // Применить нативный уровень для одного оружия из текущего владения игрока.
    // Резолвит игрока (offline → no-op), валидирует оружие. Зовётся на load и
    // на level-up — обе точки событийные, не на каждый выстрел.
    void applyNativeSkill(int playerId, std::uint8_t weaponId);

    WeaponProficiencyService &m_proficiencyService;
    PlayerSessionService &m_sessionService;
    WeaponSkillService &m_weaponSkillService;
    PlayerDialogService &m_dialogService;

    // true — async-загрузка скиллов из БД для слота успешно завершилась. Пока
    // false (загрузка не дошла: сбой/хиккап БД, дисконнект до колбэка), на конце
    // сессии НЕ персистим: иначе нулевой слот затёр бы реальный прогресс в БД.
    std::array<bool, MAX_PLAYERS> m_loaded{};
};
