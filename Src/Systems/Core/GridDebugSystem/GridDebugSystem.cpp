#include "Systems/Core/GridDebugSystem/GridDebugSystem.h"

#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <cmath>
#include <fmt/format.h>

namespace
{
// Параметры тестового поля. Дистанции стрима сознательно МЕНЬШЕ дальности
// прорисовки моделей, чтобы появление/исчезновение было видно глазами как
// работа стримера, а не прорисовки клиента.
constexpr float FIELD_HALF = 480.0f;          // поле ±480 м вокруг игрока
constexpr float OBJECT_SPACING = 60.0f;       // конус каждые 60 м (17×17 = 289 шт)
constexpr float PICKUP_SPACING = 120.0f;      // пикап каждые 120 м (9×9 = 81 шт)
constexpr float ICON_SPACING = 240.0f;        // иконка каждые 240 м (5×5 = 25 шт)
constexpr int OBJECT_MODEL = 1238;            // дорожный конус
constexpr int PICKUP_MODEL = 1240;            // сердце (аптечка)
constexpr float OBJECT_STREAM_DIST = 120.0f;
constexpr float PICKUP_STREAM_DIST = 100.0f;
constexpr float ICON_STREAM_DIST = 250.0f;

const Colour DEBUG_COLOUR{255, 220, 100};
} // namespace

GridDebugSystem::GridDebugSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_gridService(serviceRegister.getService<GridService>()),
      m_streamerService(serviceRegister.getService<StreamerService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>())
{
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add("gridtest", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { buildTestField(player); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "построить тестовое поле объектов вокруг себя",
                 PlayerCommandService::HelpCategory::Hidden);

    commands.add("gridclear", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { clearTestField(player); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "убрать тестовое поле объектов",
                 PlayerCommandService::HelpCategory::Hidden);

    commands.add("gridcell", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     DebugState &state = m_state[player.getID()];
                     state.cellNotify = !state.cellNotify;
                     state.lastCx = -1;
                     player.sendClientMessage(
                         DEBUG_COLOUR, u(state.cellNotify ? "Уведомления о ячейках: ВКЛ" : "Уведомления о ячейках: выкл"));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL),
                 "включить или выключить уведомления о смене ячейки", PlayerCommandService::HelpCategory::Hidden);

    commands.add("streamdebug", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     DebugState &state = m_state[player.getID()];
                     state.streamNotify = !state.streamNotify;
                     player.sendClientMessage(
                         DEBUG_COLOUR, u(state.streamNotify ? "Стрим-уведомления: ВКЛ" : "Стрим-уведомления: выкл"));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL),
                 "включить или выключить уведомления стримера", PlayerCommandService::HelpCategory::Hidden);

    commands.add("near", {{PlayerCommandService::Param::Int, "радиус"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 { reportNear(player, args.getInt(0)); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "показать сущности в заданном радиусе",
                 PlayerCommandService::HelpCategory::Hidden);
}

void GridDebugSystem::buildTestField(IPlayer &player)
{
    if (!m_testObjects.empty())
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Тестовое поле уже построено. Сначала /gridclear"));
        return;
    }

    const Vector3 center = m_locationService.getPosition(player.getID());

    for (float dx = -FIELD_HALF; dx <= FIELD_HALF; dx += OBJECT_SPACING)
    {
        for (float dy = -FIELD_HALF; dy <= FIELD_HALF; dy += OBJECT_SPACING)
        {
            m_testObjects.push_back(m_streamerService.addObject(
                OBJECT_MODEL, {center.x + dx, center.y + dy, center.z}, {0.0f, 0.0f, 0.0f}, OBJECT_STREAM_DIST));
        }
    }

    for (float dx = -FIELD_HALF; dx <= FIELD_HALF; dx += PICKUP_SPACING)
    {
        for (float dy = -FIELD_HALF; dy <= FIELD_HALF; dy += PICKUP_SPACING)
        {
            m_testPickups.push_back(m_streamerService.addPickup(
                PICKUP_MODEL, 1, {center.x + dx, center.y + dy, center.z + 0.5f}, 0, PICKUP_STREAM_DIST));
        }
    }

    for (float dx = -FIELD_HALF; dx <= FIELD_HALF; dx += ICON_SPACING)
    {
        for (float dy = -FIELD_HALF; dy <= FIELD_HALF; dy += ICON_SPACING)
        {
            m_testIcons.push_back(m_streamerService.addMapIcon(0, {center.x + dx, center.y + dy, center.z},
                                                               Colour{255, 60, 60}, MapIconStyle_Local,
                                                               ICON_STREAM_DIST));
        }
    }

    // Сразу включаем оба режима уведомлений — для этого тест и строится.
    DebugState &state = m_state[player.getID()];
    state.cellNotify = true;
    state.streamNotify = true;
    state.lastCx = -1;

    player.sendClientMessage(
        DEBUG_COLOUR,
        u(fmt::format("Поле построено: {} конусов (стрим {:.0f}м), {} пикапов ({:.0f}м), {} иконок ({:.0f}м)",
                      m_testObjects.size(), OBJECT_STREAM_DIST, m_testPickups.size(), PICKUP_STREAM_DIST,
                      m_testIcons.size(), ICON_STREAM_DIST)));
    player.sendClientMessage(DEBUG_COLOUR,
                             u(fmt::format("Ячейка сетки {:.0f}м. Беги/езжай — смотри сообщения. /near 150 — кто рядом",
                                           GridService::cellSize())));
}

void GridDebugSystem::clearTestField(IPlayer &player)
{
    for (int defId : m_testObjects)
        m_streamerService.removeObject(defId);
    for (int defId : m_testPickups)
        m_streamerService.removePickup(defId);
    for (int defId : m_testIcons)
        m_streamerService.removeMapIcon(defId);

    const std::size_t total = m_testObjects.size() + m_testPickups.size() + m_testIcons.size();
    m_testObjects.clear();
    m_testPickups.clear();
    m_testIcons.clear();

    player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Тестовое поле убрано ({} сущностей)", total)));
}

void GridDebugSystem::reportNear(IPlayer &player, int radius)
{
    if (radius <= 0 || radius > 1000)
        radius = 300;

    int players = 0;
    int vehicles = 0;
    int objects = 0;
    int pickups = 0;
    int icons = 0;
    float nearestPlayerSq = -1.0f;

    m_gridService.forEachInRadius(m_locationService.getPosition(player.getID()), static_cast<float>(radius),
                                  GRID_MASK_ALL,
                                  [&](GridEntityType type, std::int32_t id, float distSq)
                                  {
                                      switch (type)
                                      {
                                      case GridEntityType::Player:
                                          if (id != player.getID())
                                          {
                                              ++players;
                                              if (nearestPlayerSq < 0.0f || distSq < nearestPlayerSq)
                                                  nearestPlayerSq = distSq;
                                          }
                                          break;
                                      case GridEntityType::Vehicle:
                                          ++vehicles;
                                          break;
                                      case GridEntityType::Object:
                                          ++objects;
                                          break;
                                      case GridEntityType::Pickup:
                                          ++pickups;
                                          break;
                                      case GridEntityType::MapIcon:
                                          ++icons;
                                          break;
                                      default:
                                          break;
                                      }
                                  });

    player.sendClientMessage(
        DEBUG_COLOUR, u(fmt::format("В радиусе {}м: игроков {}, машин {}, объектов {}, пикапов {}, иконок {}", radius,
                                    players, vehicles, objects, pickups, icons)));
    if (nearestPlayerSq >= 0.0f)
    {
        player.sendClientMessage(
            DEBUG_COLOUR, u(fmt::format("Ближайший игрок: {:.1f}м", std::sqrt(nearestPlayerSq))));
    }
}

bool GridDebugSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    DebugState &state = m_state[player.getID()];

    if (state.cellNotify)
    {
        const GridService::CellCoords cell = GridService::cellOf(m_locationService.getPosition(player.getID()));
        if (cell.cx != state.lastCx || cell.cy != state.lastCy)
        {
            if (state.lastCx >= 0)
            {
                player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Ячейка ({};{}) -> ({};{})", state.lastCx,
                                                                     state.lastCy, cell.cx, cell.cy)));
            }
            state.lastCx = cell.cx;
            state.lastCy = cell.cy;
        }
    }

    if (state.streamNotify)
    {
        const std::size_t objects = m_streamerService.shownObjectCount(player.getID());
        const std::size_t icons = m_streamerService.shownIconCount(player.getID());
        const int pickups = m_streamerService.activePickupCount();
        if (objects != state.lastObjects || icons != state.lastIcons || pickups != state.lastPickups)
        {
            player.sendClientMessage(
                DEBUG_COLOUR, u(fmt::format("Стрим: объектов {} ({:+}), иконок {} ({:+}), пикапов в пуле {} ({:+})",
                                            objects, static_cast<int>(objects) - static_cast<int>(state.lastObjects),
                                            icons, static_cast<int>(icons) - static_cast<int>(state.lastIcons),
                                            pickups, pickups - state.lastPickups)));
            state.lastObjects = objects;
            state.lastIcons = icons;
            state.lastPickups = pickups;
        }
    }

    return true;
}

void GridDebugSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_state[player.getID()] = DebugState{};
}
