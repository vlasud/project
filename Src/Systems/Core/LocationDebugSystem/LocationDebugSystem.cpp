#include "Systems/Core/LocationDebugSystem/LocationDebugSystem.h"

#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include "glm/geometric.hpp"
#include <chrono>
#include <fmt/format.h>

namespace
{
constexpr std::chrono::milliseconds VEL_NOTIFY_INTERVAL{500};

const Colour DEBUG_COLOUR{120, 220, 255};

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

const char *violationName(AntiCheatService::ViolationType type)
{
    switch (type)
    {
    case AntiCheatService::ViolationType::ForcedAnimationEscape:
        return "AnimEscape";
    case AntiCheatService::ViolationType::HealthHack:
        return "HealthHack";
    case AntiCheatService::ViolationType::DamageHack:
        return "DamageHack";
    case AntiCheatService::ViolationType::DeathEvasion:
        return "DeathEvasion";
    case AntiCheatService::ViolationType::TeleportHack:
        return "TeleportHack";
    case AntiCheatService::ViolationType::SpeedHack:
        return "SpeedHack";
    case AntiCheatService::ViolationType::StateHack:
        return "StateHack";
    case AntiCheatService::ViolationType::SpecialActionHack:
        return "SpecialActionHack";
    case AntiCheatService::ViolationType::WeaponHack:
        return "WeaponHack";
    case AntiCheatService::ViolationType::VehicleHack:
        return "VehicleHack";
    }
    return "Unknown";
}
} // namespace

LocationDebugSystem::LocationDebugSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_velocityService(serviceRegister.getService<PlayerVelocityService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>())
{
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add("pos", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showPos(player); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    commands.add("tp", {{PlayerCommandService::Param::Int, "x"}, {PlayerCommandService::Param::Int, "y"},
                        {PlayerCommandService::Param::Int, "z"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     const Vector3 target{static_cast<float>(args.getInt(0)), static_cast<float>(args.getInt(1)),
                                          static_cast<float>(args.getInt(2))};
                     m_locationService.teleport(player, target);
                     player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Телепорт в {:.0f} {:.0f} {:.0f}. "
                                                                          "Нарушений быть не должно — /violations",
                                                                          target.x, target.y, target.z)));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    commands.add("tpup", {{PlayerCommandService::Param::Int, "метры"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     int meters = args.getInt(0);
                     if (meters < 1 || meters > 1000)
                         meters = 300;
                     Vector3 target = m_locationService.getPosition(player.getID());
                     target.z += static_cast<float>(meters);
                     m_locationService.teleport(player, target);
                     player.sendClientMessage(
                         DEBUG_COLOUR, u(fmt::format("Подброшен на {} м — свободное падение. Включи /veldebug и "
                                                     "смотри вертикальную скорость; SpeedHack быть не должно",
                                                     meters)));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    commands.add("vw", {{PlayerCommandService::Param::Int, "id"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     m_locationService.setVirtualWorld(player, args.getInt(0));
                     player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Виртуальный мир: {}", args.getInt(0))));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    commands.add("int", {{PlayerCommandService::Param::Int, "id"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     const int interior = args.getInt(0) < 0 ? 0 : args.getInt(0);
                     m_locationService.setInterior(player, static_cast<unsigned>(interior));
                     player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Интерьер: {}", interior)));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    commands.add("hackpos", {{PlayerCommandService::Param::Int, "метры"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     // Меньше 50 м не имеет смысла: короткий скачок укладывается в
                     // скоростной допуск валидатора (50 м/с × dt + 10 м) и легален.
                     int meters = args.getInt(0);
                     if (meters < 50 || meters > 10000)
                         meters = 200;
                     // НАМЕРЕННО сырой setPosition мимо сервиса — имитация
                     // телепорт-хака. Валидатор обязан откатить и записать нарушение.
                     Vector3 target = m_locationService.getPosition(player.getID());
                     target.x += static_cast<float>(meters);
                     player.setPosition(target);
                     player.sendClientMessage(
                         DEBUG_COLOUR, u(fmt::format("Сырой скачок на {} м: жди отката на место и TeleportHack "
                                                     "в /violations",
                                                     meters)));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    commands.add("vel", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showVel(player); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    commands.add("veldebug", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     DebugState &state = m_state[player.getID()];
                     state.velNotify = !state.velNotify;
                     player.sendClientMessage(DEBUG_COLOUR,
                                              u(state.velNotify ? "Поток скорости: ВКЛ" : "Поток скорости: выкл"));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    commands.add("violations", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showViolations(player); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    commands.add("acclear", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     // Чистка собственного журнала между тестами — чтобы серия
                     // намеренных нарушений не добиралась до порога кика.
                     m_antiCheatService.clear(player.getID());
                     player.sendClientMessage(DEBUG_COLOUR, u("Журнал нарушений очищен"));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));
}

void LocationDebugSystem::showPos(IPlayer &player)
{
    const int playerId = player.getID();
    const Vector3 accepted = m_locationService.getPosition(playerId);
    // Сырое чтение здесь намеренно: смысл команды — сравнить правду сервера с
    // заявлением клиента (после отката они расходятся до прибытия).
    const Vector3 raw = player.getPosition();
    const GridService::CellCoords cell = GridService::cellOf(accepted);

    player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Принято: {:.1f} {:.1f} {:.1f} | ячейка ({};{})", accepted.x,
                                                         accepted.y, accepted.z, cell.cx, cell.cy)));
    player.sendClientMessage(
        DEBUG_COLOUR, u(fmt::format("Клиент:  {:.1f} {:.1f} {:.1f} | расхождение {:.2f} м", raw.x, raw.y, raw.z,
                                    glm::distance(accepted, raw))));
    player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Интерьер {} | виртуальный мир {}",
                                                         m_locationService.getInterior(playerId),
                                                         m_locationService.getVirtualWorld(playerId))));
}

void LocationDebugSystem::showVel(IPlayer &player)
{
    const int playerId = player.getID();
    const Vector3 server = m_velocityService.getVelocity(playerId);
    const Vector3 client = player.getVelocity(); // намеренно: сравнение с заявлением клиента

    player.sendClientMessage(
        DEBUG_COLOUR, u(fmt::format("Сервер: {:.1f} м/с (гориз {:.1f}, верт {:+.1f})", m_velocityService.getSpeed(playerId),
                                    m_velocityService.getHorizontalSpeed(playerId),
                                    m_velocityService.getVerticalSpeed(playerId))));
    // Клиентская velocity в sync — в единицах за кадр; домножать на тикрейт не
    // будем, показываем как есть для сопоставления направления.
    player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Клиент (сырое поле sync): {:.3f} {:.3f} {:.3f}", client.x,
                                                         client.y, client.z)));
}

void LocationDebugSystem::showViolations(IPlayer &player)
{
    const AntiCheatService::PlayerRecord &record = m_antiCheatService.get(player.getID());
    if (record.total == 0)
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Нарушений нет"));
        return;
    }

    player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Нарушений всего: {}", record.total)));
    const std::size_t shown = record.recent.size() < 5 ? record.recent.size() : 5;
    for (std::size_t i = record.recent.size() - shown; i < record.recent.size(); ++i)
    {
        const AntiCheatService::Violation &violation = record.recent[i];
        player.sendClientMessage(
            DEBUG_COLOUR, u(fmt::format("[{}] {}", violationName(violation.type), violation.detail)));
    }
}

bool LocationDebugSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    DebugState &state = m_state[player.getID()];
    if (state.velNotify && now >= state.nextVelAt)
    {
        state.nextVelAt = now + VEL_NOTIFY_INTERVAL;
        const int playerId = player.getID();
        player.sendClientMessage(
            DEBUG_COLOUR,
            u(fmt::format("v {:.1f} м/с | гориз {:.1f} | верт {:+.1f}", m_velocityService.getSpeed(playerId),
                          m_velocityService.getHorizontalSpeed(playerId),
                          m_velocityService.getVerticalSpeed(playerId))));
    }
    return true;
}

void LocationDebugSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_state[player.getID()] = DebugState{};
}
