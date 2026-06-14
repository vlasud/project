#include "Systems/PlayerSessionSystem/PlayerSessionSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <fmt/format.h>

namespace
{
const Colour DEBUG_COLOUR{120, 220, 255};

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

PlayerSessionSystem::PlayerSessionSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    serviceRegister.getService<PlayerCommandService>().add(
        "session", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            const PlayerSessionService::Session *session = m_sessionService.get(player.getID());
            if (!session)
            {
                player.sendClientMessage(DEBUG_COLOUR, u("Сессии нет — вы не авторизованы"));
                return;
            }
            const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now() -
                                                                                  session->startedAt);
            player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Аккаунт #{} | сессия №{} | в игре {} мин",
                                                                 session->accountId, session->serial,
                                                                 minutes.count())));
        },
        {}, "показать данные своей сессии: аккаунт, время в игре", PlayerCommandService::HelpCategory::Hidden);
}

void PlayerSessionSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_sessionService.end(player);
}
