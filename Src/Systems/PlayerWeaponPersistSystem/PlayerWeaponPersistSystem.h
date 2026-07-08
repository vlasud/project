#pragma once

#include "Macro.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/PlayerWeaponPersistService/PlayerWeaponPersistService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Персист оружия (`player_weapon`) аккаунта. Бизнес-фича персиста стартовой
// экипировки: ВЫДАЧА ОРУЖИЯ (разовая, на логин-спавне) остаётся в
// PlayerAuthSystem (m_pendingSpawnSetup) — эта система только грузит/
// сохраняет данные и кладёт загруженное в PlayerWeaponPersistService (см.
// Docs/Persistence.md).
//
//  * коннект/дисконнект — сброс кэша слота, чтобы данные одного аккаунта не
//    утекли следующему игроку в том же playerId-слоте;
//  * старт сессии (subscribeStart) — async-select по account_id с
//    serial-guard (как InventorySystem/SpawnChoiceSystem). Оружие
//    сбрасывается на КАЖДОМ спавне — результат кладётся только в кэш
//    (loadWeapons), giveWeapon реально прогонит PlayerAuthSystem на
//    логин-спавне (см. PlayerAuthSystem — late-применение, если спавн уже
//    прошёл раньше загрузки);
//  * сохранение (subscribeSave) — снимок ТЕКУЩЕГО состояния
//    (PlayerWeaponService::getWeapons) идемпотентным REPLACE. Гейтится
//    areWeaponsApplied, а НЕ areWeaponsLoaded: между приходом кэша и
//    логин-спавном рантайм-инвентарь ещё пуст, и снимок этой пустоты затёр бы
//    реальное оружие в БД, сорвись сессия до спавна. Зовётся на конце сессии
//    И периодически автосейвом (см. Docs/Autosave.md).
//  * КАВЕАТ: оружие теряется на смерти (GTA) и восстанавливается ТОЛЬКО на
//    логин-спавне — снимок на save содержит то оружие, что у игрока живым
//    прямо сейчас (см. Docs/Persistence.md).
class PlayerWeaponPersistSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    PlayerWeaponPersistSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    void loadWeapons(IPlayer &player, const PlayerSessionService::Session &session);
    void persistWeapons(IPlayer &player, const PlayerSessionService::Session &session);

    PlayerWeaponPersistService &m_persistService;
    PlayerWeaponService &m_weaponService;
    PlayerSessionService &m_sessionService;
};
