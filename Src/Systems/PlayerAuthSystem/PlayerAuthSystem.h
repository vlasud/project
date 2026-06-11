#pragma once

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
#include "Services/PlayerAuthService/PlayerAuthService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Classes/classes.hpp>
#include <cstdint>

class PlayerAuthSystem : public BaseSystem,
                         public PlayerConnectEventHandler,
                         public PlayerChangeEventHandler,
                         public ClassEventHandler,
                         public PlayerSpawnEventHandler
{
  public:
    PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

    void onPlayerKeyStateChange(IPlayer &player, uint32_t newKeys, uint32_t oldKeys) override;

    bool onPlayerRequestClass(IPlayer &player, unsigned int classId) override;

    bool onPlayerRequestSpawn(IPlayer &player) override;
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
        std::string passwordHash;
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
    void finalize(IPlayer &player);

    PlayerAuthService &m_authService;
    PlayerConnectionVersionService &m_connectionVersionService;
    PlayerDialogService &m_dialogService;
    PlayerLocationService &m_locationService;
    PlayerStateService &m_stateService;
    PlayerWeaponService &m_weaponService;
    PlayerMoneyService &m_moneyService;

    std::array<LoginData, MAX_PLAYERS> m_loginData;
    std::array<RegistrationData, MAX_PLAYERS> m_registrationData;

    // setSpectating(false) в finalize() вызывает респаун; экипировку и телепорт
    // нельзя делать до его события спавна — сервисы на спавне сбрасывают
    // инвентарь и ожидание телепорта. Флаг переносит настройку в onPlayerSpawn.
    std::array<bool, MAX_PLAYERS> m_pendingSpawnSetup{};

    // Первый onPlayerRequestClass — автоматический вход клиента в класс-селекшн;
    // все последующие — нажатия дефолтных стрелок ◄ ► внизу экрана.
    std::array<bool, MAX_PLAYERS> m_classSelectionEntered{};
};
