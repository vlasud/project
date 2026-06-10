#include "SystemRegister.h"

#include "AntiCheatSystem/AntiCheatSystem.h"
#include "ChatSystem/ChatSystem.h"
#include "DebugCameraSystem/DebugCameraSystem.h"
#include "EditorSystem/EditorSystem.h"
#include "GridDebugSystem/GridDebugSystem.h"
#include "GridSystem/GridSystem.h"
#include "LocationDebugSystem/LocationDebugSystem.h"
#include "PlayerAnimationSystem/PlayerAnimationSystem.h"
#include "PlayerAuthSystem/PlayerAuthSystem.h"
#include "PlayerCommandSystem/PlayerCommandSystem.h"
#include "PlayerConnectionVersionSystem/PlayerConnectionVersionSystem.h"
#include "PlayerDialogSystem/PlayerDialogSystem.h"
#include "PlayerHealthSystem/PlayerHealthSystem.h"
#include "PlayerLocationSystem/PlayerLocationSystem.h"
#include "PlayerStateSystem/PlayerStateSystem.h"
#include "PlayerVelocitySystem/PlayerVelocitySystem.h"
#include "PlayerWeaponSystem/PlayerWeaponSystem.h"
#include "StreamerSystem/StreamerSystem.h"
#include "VehicleDebugSystem/VehicleDebugSystem.h"
#include "VehicleSystem/VehicleSystem.h"

void SystemRegister::registerSystems(ICore &core, const ServiceRegister &serviceRegister)
{
    m_systems.push_back(std::make_unique<PlayerConnectionVersionSystem>(core, serviceRegister));
    // LocationSystem раньше остальных: на том же апдейте все читают уже принятую
    // позицию; VelocitySystem сразу после — производная от свежей позиции.
    m_systems.push_back(std::make_unique<PlayerLocationSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerStateSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerVelocitySystem>(core, serviceRegister));
    // VehicleSystem раньше GridSystem: на смене стейта привязка пассажира уже
    // сделана, грид читает машину игрока из источника правды.
    m_systems.push_back(std::make_unique<VehicleSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<GridSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<StreamerSystem>(core, serviceRegister));
    // m_systems.push_back(std::make_unique<GridDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<LocationDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<VehicleDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerDialogSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerCommandSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<AntiCheatSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerAnimationSystem>(core, serviceRegister));
    // WeaponSystem раньше HealthSystem: фейковый выстрел (оружие без выдачи)
    // отбрасывается до регистрации bullet sync в health — не легализует give-damage.
    m_systems.push_back(std::make_unique<PlayerWeaponSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerHealthSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerAuthSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<DebugCameraSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<EditorSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<ChatSystemSystem>(core, serviceRegister));
}

void SystemRegister::initializeSystems(IComponentList *components)
{
    for (const auto &system : m_systems)
    {
        system->initialize(components);
    }
}

void SystemRegister::resetSystems()
{
    for (const auto &system : m_systems)
    {
        system->reset();
    }
}
