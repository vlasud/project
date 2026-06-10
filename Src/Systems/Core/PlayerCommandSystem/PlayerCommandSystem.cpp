#include "Systems/Core/PlayerCommandSystem/PlayerCommandSystem.h"
#include "Utils/Encoding/Encoding.h"

namespace
{
const std::string ERROR_MESSAGE = Encoding::utf8Tocp1251("Неизвестная команда...");
}

PlayerCommandSystem::PlayerCommandSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_commandService(serviceRegister.getService<PlayerCommandService>())
{
    core.getPlayers().getPlayerTextDispatcher().addEventHandler(this);
}

bool PlayerCommandSystem::onPlayerCommandText(IPlayer &player, StringView message)
{
    const bool result = m_commandService.dispatch(player, message);
    if (!result)
    {
        player.sendClientMessage(Colour::White(), ERROR_MESSAGE);
    }
    return true;
}
