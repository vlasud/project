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
#include "PlayerVelocitySystem/PlayerVelocitySystem.h"
#include "StreamerSystem/StreamerSystem.h"

void SystemRegister::registerSystems(ICore &core, const ServiceRegister &serviceRegister)
{
    m_systems.push_back(std::make_unique<PlayerConnectionVersionSystem>(core, serviceRegister));
    // LocationSystem раньше остальных: на том же апдейте все читают уже принятую
    // позицию; VelocitySystem сразу после — производная от свежей позиции.
    m_systems.push_back(std::make_unique<PlayerLocationSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerVelocitySystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<GridSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<StreamerSystem>(core, serviceRegister));
    // m_systems.push_back(std::make_unique<GridDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<LocationDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerDialogSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerCommandSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<AntiCheatSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerAnimationSystem>(core, serviceRegister));
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
