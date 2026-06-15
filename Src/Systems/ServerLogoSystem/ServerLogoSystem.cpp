#include "Systems/ServerLogoSystem/ServerLogoSystem.h"

#include "Log/LogManager.h"

ServerLogoSystem::ServerLogoSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_textDrawService(serviceRegister.getService<TextDrawService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void ServerLogoSystem::initialize(IComponentList *components)
{
    // Компонент textdraw может быть не загружен — без него логотип не создаём.
    if (!m_textDrawService.isAvailable())
    {
        LogManager::log(Warning, "ServerLogoSystem: textdraw component is unavailable, server logo disabled");
        return;
    }

    Vector2 position{533.0f, 4.0f};

    TextDrawParams params;
    params.alignment = TextDrawAlignment_Center;
    params.style = TextDrawStyle_1;
    params.letterSize = Vector2(0.4f, 1.6f);
    params.textSize = Vector2(400.0f, 17.0f);
    params.letterColour = Colour::FromRGBA(0xFFFFFFFF);
    params.box = false;
    params.boxColour = Colour::FromRGBA(0x00000080);
    params.backgroundColour = Colour::FromRGBA(0x000000FF);
    params.proportional = true;
    params.selectable = false;
    params.shadow = 0;
    params.outline = 0;

    // create сам санитизирует текст и клампит параметры; пул textdraw конечен —
    // при исчерпании вернётся nullptr, тогда m_logoId остаётся -1.
    ITextDraw *logo = m_textDrawService.create(position, "HARDWAY", params);
    if (!logo)
    {
        LogManager::log(Warning, "ServerLogoSystem: failed to create server logo (textdraw pool exhausted?)");
        return;
    }

    m_logoId = logo->getID();
}

void ServerLogoSystem::onPlayerConnect(IPlayer &player)
{
    // Глобальный textdraw невидим, пока не показан; включаем его каждому на входе.
    if (m_logoId >= 0)
    {
        m_textDrawService.showForPlayer(player, m_logoId);
    }
}
