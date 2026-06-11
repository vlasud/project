#include "Systems/Core/TextDrawSystem/TextDrawSystem.h"
#include "Log/LogManager.h"

TextDrawSystem::TextDrawSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_textDrawService(serviceRegister.getService<TextDrawService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void TextDrawSystem::initialize(IComponentList *components)
{
    ITextDrawsComponent *textDraws = components->queryComponent<ITextDrawsComponent>();
    if (!textDraws)
    {
        LogManager::log(Error, "TextDrawSystem: ITextDrawsComponent is missing, textdraws are disabled");
        return;
    }

    m_textDrawService.initialize(textDraws);
    textDraws->getEventDispatcher().addEventHandler(this);
}

void TextDrawSystem::onPlayerClickTextDraw(IPlayer &player, ITextDraw &textDraw)
{
    m_textDrawService.handleClick(player, textDraw);
}

void TextDrawSystem::onPlayerClickPlayerTextDraw(IPlayer &player, IPlayerTextDraw &textDraw)
{
    m_textDrawService.handlePlayerClick(player, textDraw);
}

bool TextDrawSystem::onPlayerCancelTextDrawSelection(IPlayer &player)
{
    return m_textDrawService.handleCancelSelection(player);
}

bool TextDrawSystem::onPlayerCancelPlayerTextDrawSelection(IPlayer &player)
{
    return m_textDrawService.handleCancelSelection(player);
}

void TextDrawSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_textDrawService.resetPlayer(player.getID());
}
