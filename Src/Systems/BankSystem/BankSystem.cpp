#include "Systems/BankSystem/BankSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
} // namespace

BankSystem::BankSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_bankService(serviceRegister.getService<BankService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    serviceRegister.getService<PlayerCommandService>().add(
        "balance", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            const PlayerSessionService::Session *session = m_sessionService.get(player.getID());
            if (!session)
            {
                player.sendClientMessage(INFO_COLOUR, u("Вы не авторизованы"));
                return;
            }
            m_bankService.getBalance(
                session->accountId,
                [this, playerId = player.getID(), serial = session->serial](std::int64_t balance)
                {
                    // Serial-guard: ответ БД мог прилететь уже другой сессии.
                    const PlayerSessionService::Session *current = m_sessionService.get(playerId);
                    if (!current || current->serial != serial)
                        return;
                    if (IPlayer *player = m_core.getPlayers().get(playerId))
                        player->sendClientMessage(INFO_COLOUR,
                                                  u(fmt::format("Счёт в банке: ${}", balance)));
                });
        },
        {}, "показать баланс банковского счёта", PlayerCommandService::HelpCategory::Economy);
}
