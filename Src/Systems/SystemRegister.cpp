#include "SystemRegister.h"

#include "PlayerAuthSystem/PlayerAuthSystem.h"
#include "PlayerConnectionVersionSystem/PlayerConnectionVersionSystem.h"
#include "PlayerDialogSystem/PlayerDialogSystem.h"

void SystemRegister::registerSystems(ICore &core, const ServiceRegister &serviceRegister)
{
    m_systems.push_back(std::make_unique<PlayerConnectionVersionSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerDialogSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerAuthSystem>(core, serviceRegister));
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
