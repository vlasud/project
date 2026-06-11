#include "Systems/SystemRegister.h"

#include "Systems/Core/AntiCheatSystem/AntiCheatSystem.h"
#include "Systems/Core/ChatSystem/ChatSystem.h"
#include "Systems/Core/DebugCameraSystem/DebugCameraSystem.h"
#include "Systems/Core/EditorSystem/EditorSystem.h"
#include "Systems/Core/GridDebugSystem/GridDebugSystem.h"
#include "Systems/Core/GridSystem/GridSystem.h"
#include "Systems/Core/LocationDebugSystem/LocationDebugSystem.h"
#include "Systems/Core/PlayerAnimationSystem/PlayerAnimationSystem.h"
#include "Systems/PlayerAuthSystem/PlayerAuthSystem.h"
#include "Systems/Core/PlayerCommandSystem/PlayerCommandSystem.h"
#include "Systems/Core/PlayerConnectionVersionSystem/PlayerConnectionVersionSystem.h"
#include "Systems/Core/PlayerDialogSystem/PlayerDialogSystem.h"
#include "Systems/Core/PlayerHealthSystem/PlayerHealthSystem.h"
#include "Systems/Core/PlayerLocationSystem/PlayerLocationSystem.h"
#include "Systems/Core/PlayerMoneySystem/PlayerMoneySystem.h"
#include "Systems/Core/PlayerStateSystem/PlayerStateSystem.h"
#include "Systems/Core/PlayerVelocitySystem/PlayerVelocitySystem.h"
#include "Systems/Core/PlayerWeaponSystem/PlayerWeaponSystem.h"
#include "Systems/Core/StreamerSystem/StreamerSystem.h"
#include "Systems/Core/VehicleDebugSystem/VehicleDebugSystem.h"
#include "Systems/Core/VehicleSystem/VehicleSystem.h"

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
    m_systems.push_back(std::make_unique<PlayerMoneySystem>(core, serviceRegister));
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
