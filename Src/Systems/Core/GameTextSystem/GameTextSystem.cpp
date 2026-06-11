#include "Systems/Core/GameTextSystem/GameTextSystem.h"

GameTextSystem::GameTextSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister)
{
    serviceRegister.getService<GameTextService>().initialize(&core);
}
