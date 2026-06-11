#include "Systems/Core/TextLabelSystem/TextLabelSystem.h"

#include "Services/Core/StreamerService/StreamerService.h"

TextLabelSystem::TextLabelSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister)
{
    serviceRegister.getService<TextLabelService>().initialize(&serviceRegister.getService<StreamerService>());
}
