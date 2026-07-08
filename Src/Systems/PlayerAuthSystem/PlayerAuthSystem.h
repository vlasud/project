#pragma once

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
#include "Services/Core/PlayerSkinService/PlayerSkinService.h"
#include "Services/PlayerMoneyPersistService/PlayerMoneyPersistService.h"
#include "Services/PlayerWeaponPersistService/PlayerWeaponPersistService.h"
#include "Services/PlayerPersonalSkinService/PlayerPersonalSkinService.h"
#include "Services/PlayerAuthService/PlayerAuthService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/PlayerSpawnService/PlayerSpawnService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <cstdint>

class PlayerAuthSystem : public BaseSystem,
                         public PlayerConnectEventHandler,
                         public PlayerChangeEventHandler,
                         public PlayerSpawnEventHandler
{
  public:
    PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

    void onPlayerKeyStateChange(IPlayer &player, uint32_t newKeys, uint32_t oldKeys) override;

    void onPlayerSpawn(IPlayer &player) override;

  private:
    enum class ESex : uint8_t
    {
        MALE = 0,
        FEMALE
    };

    struct LoginData
    {
        int loginAttempts = 0;
        PlayerSessionService::AccountId accountId = PlayerSessionService::NO_ACCOUNT;
        std::string passwordHash;
        // Личный (гражданский) скин аккаунта из БД. Применяется в finalize ДО
        // старта членства; валидируется PlayerPersonalSkinService.
        int personalSkin = PlayerPersonalSkinService::DEFAULT_SKIN_MALE;
    };

    struct RegistrationData
    {
        ESex sex = ESex::MALE;
        std::string password;
    };

    void resetState(int playerId);
    void runRegistration(int playerId);
    void runLogin(int playerId);
    void runChooseSex(int playerId);
    void runSelectSkin(int playerId);
    void showLoginDialog(IPlayer &player);
    void showRegistrationPasswordDialog(IPlayer &player);
    void showRegistrationConfirmDialog(IPlayer &player);
    void showChooseSexDialog(IPlayer &player);
    void finalizeRegistration(IPlayer &player);
    // personalSkin — личный скин аккаунта (из БД при логине / дефолт по полу
    // при регистрации). Применяется через PlayerSkinService и сидируется в
    // PlayerPersonalSkinService ДО того, как async-загрузка членства наложит
    // органный скин и захватит «гражданский» для возврата.
    void finalize(IPlayer &player, int personalSkin);

    // Единственная точка ВЫДАЧИ ОРУЖИЯ на логин-спавне (наличные к этому моменту
    // обычно уже применены на загрузке — см. PlayerMoneyPersistSystem — этот
    // вызов лишь ресинхронит HUD). Если кэш PlayerMoneyPersistService/
    // PlayerWeaponPersistService к этому моменту ещё не загружен (гонка с
    // async-загрузкой) — взводит m_awaitingMoneyApply/m_awaitingWeaponsApply, и
    // применение довершат наблюдатели subscribeMoneyLoaded/subscribeWeaponsLoaded
    // из конструктора.
    void applyPersistedEquipment(IPlayer &player);

    PlayerAuthService &m_authService;
    PlayerConnectionVersionService &m_connectionVersionService;
    PlayerDialogService &m_dialogService;
    PlayerLocationService &m_locationService;
    PlayerStateService &m_stateService;
    PlayerWeaponService &m_weaponService;
    PlayerMoneyService &m_moneyService;
    PlayerMoneyPersistService &m_moneyPersistService;
    PlayerWeaponPersistService &m_weaponPersistService;
    PlayerSpawnService &m_spawnService;
    PlayerSkinService &m_skinService;
    PlayerPersonalSkinService &m_personalSkinService;
    PlayerSessionService &m_sessionService;

    std::array<LoginData, MAX_PLAYERS> m_loginData;
    std::array<RegistrationData, MAX_PLAYERS> m_registrationData;

    // setSpectating(false) в finalize() вызывает респаун; экипировку и телепорт
    // нельзя делать до его события спавна — сервисы на спавне сбрасывают
    // инвентарь и ожидание телепорта. Флаг переносит настройку в onPlayerSpawn.
    std::array<bool, MAX_PLAYERS> m_pendingSpawnSetup{};

    // Спавн прошёл раньше async-загрузки персиста — apply довершит наблюдатель
    // загрузки (см. applyPersistedEquipment). Сбрасываются в resetState на
    // коннекте/дисконнекте, чтобы переиспользуемый слот не унаследовал чужое
    // ожидание.
    std::array<bool, MAX_PLAYERS> m_awaitingMoneyApply{};
    std::array<bool, MAX_PLAYERS> m_awaitingWeaponsApply{};
};
