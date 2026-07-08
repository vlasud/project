#include "Systems/Core/SpectateSystem/SpectateSystem.h"

#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <fmt/format.h>

SpectateSystem::SpectateSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_spectateService(serviceRegister.getService<SpectateService>()),
      m_timerService(serviceRegister.getService<TimerService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);

    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add("spec", {{PlayerCommandService::Param::Int, "id игрока"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     IPlayer *target = m_core.getPlayers().get(args.getInt(0));
                     if (!target)
                     {
                         player.sendClientMessage(Colour::White(), u("Игрок с таким id не в сети"));
                         return;
                     }
                     if (!m_spectateService.spectatePlayer(
                             player, *target,
                             [](IPlayer &spectator, SpectateService::StopReason reason)
                             {
                                 if (reason == SpectateService::StopReason::TargetLost)
                                 {
                                     spectator.sendClientMessage(Colour::White(),
                                                                 u("Цель вышла — спектейт завершён"));
                                 }
                             }))
                     {
                         player.sendClientMessage(Colour::White(), u("Нельзя наблюдать за собой"));
                         return;
                     }
                     player.sendClientMessage(
                         Colour::White(), u(fmt::format("Наблюдение за {} (id {}). /specoff — выйти.",
                                                        target->getName().to_string(), target->getID())));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "наблюдать за игроком по id",
                 PlayerCommandService::HelpCategory::Hidden);

    commands.add("specoff", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     if (!m_spectateService.isSpectating(player.getID()))
                     {
                         player.sendClientMessage(Colour::White(), u("Вы никого не наблюдаете"));
                         return;
                     }
                     m_spectateService.stop(player);
                     player.sendClientMessage(Colour::White(), u("Спектейт завершён"));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "выйти из режима наблюдения",
                 PlayerCommandService::HelpCategory::Hidden);
}

void SpectateSystem::initialize(IComponentList *components)
{
    m_spectateService.initialize(&m_core, &m_serviceRegister.getService<PlayerStateService>(),
                                 &m_serviceRegister.getService<PlayerLocationService>(),
                                 &m_serviceRegister.getService<VehicleService>());

    // Догон цели (интерьер/мир/тело) — троттленный свип, вне горячего пути.
    m_timerService.setInterval(std::chrono::milliseconds(500), [this] { m_spectateService.sweep(); });
}

void SpectateSystem::onPlayerSpawn(IPlayer &player)
{
    m_spectateService.handleSpawn(player);
}

void SpectateSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_spectateService.resetPlayer(player.getID());
}
