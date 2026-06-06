#include "SystemRegister.h"

#include "PlayerAuthSystem/PlayerAuthSystem.h"

void SystemRegister::registerSystems(ICore &core, const ServiceRegister &serviceRegister)
{
    m_systems.push_back(std::make_unique<PlayerAuthSystem>(core, serviceRegister));
}

void SystemRegister::initializeSystems()
{
    for (const auto &system : m_systems)
    {
        system->initialize();
    }
}

void SystemRegister::resetSystems()
{
    for (const auto &system : m_systems)
    {
        system->reset();
    }
}
