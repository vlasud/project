#pragma once

#include "Systems/BaseSystem.h"
#include <memory>
#include <vector>

class SystemRegister final
{
  public:
    void registerSystems(ICore &core, const ServiceRegister &serviceRegister);
    void initializeSystems(IComponentList *components);
    void resetSystems();

  private:
    std::vector<std::unique_ptr<BaseSystem>> m_systems;
};
