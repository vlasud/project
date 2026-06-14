#include "Systems/Core/WeaponDebugSystem/WeaponDebugSystem.h"

#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <fmt/format.h>

namespace
{
const Colour DEBUG_COLOUR{120, 220, 255};

// Пауза больше этой — новая очередь: интервал через перезарядку/перерыв не
// характеризует темп и в статистику не идёт.
constexpr std::chrono::milliseconds BURST_GAP{2000};

// Слот гейм-текста тулзы — вне классических стилей 0..6 игровых сообщений.
constexpr int GT_STYLE = 7;
constexpr Milliseconds GT_TIME{2000};

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

WeaponDebugSystem::WeaponDebugSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_gameTextService(serviceRegister.getService<GameTextService>())
{
    core.getPlayers().getPlayerShotDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    serviceRegister.getService<PlayerCommandService>().add(
        "rof", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { toggle(player); },
        PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));
}

void WeaponDebugSystem::toggle(IPlayer &player)
{
    DebugState &state = m_state[player.getID()];
    if (!state.enabled)
    {
        state = DebugState{};
        state.enabled = true;
        player.sendClientMessage(DEBUG_COLOUR, u("Замер темпа стрельбы: ВКЛ — стреляй, интервалы на экране. "
                                                 "Повторный /rof — сводка и выкл"));
        return;
    }

    showSummary(player, state);
    state = DebugState{};
}

void WeaponDebugSystem::showSummary(IPlayer &player, const DebugState &state)
{
    player.sendClientMessage(DEBUG_COLOUR, u("Темп стрельбы (мин — кандидат в таблицу анти-rapid-fire):"));
    bool any = false;
    for (std::size_t weapon = 0; weapon < state.stats.size(); ++weapon)
    {
        const WeaponStat &ws = state.stats[weapon];
        if (ws.count == 0)
            continue;
        any = true;
        player.sendClientMessage(
            DEBUG_COLOUR,
            u(fmt::format("ID {}: интервалов {}, мин {} мс, ср {} мс | таблица {} мс", weapon, ws.count, ws.minMs,
                          ws.sumMs / ws.count,
                          PlayerWeaponService::minShotInterval(static_cast<std::uint8_t>(weapon)).count())));
    }
    if (!any)
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Замеров нет: нужно минимум два выстрела подряд одним оружием"));
    }
}

bool WeaponDebugSystem::handleShot(IPlayer &player, const PlayerBulletData &bulletData)
{
    DebugState &state = m_state[player.getID()];
    if (!state.enabled)
        return true;

    const TimePoint now = std::chrono::steady_clock::now();
    const std::uint8_t weapon = bulletData.weapon;
    const auto table = PlayerWeaponService::minShotInterval(weapon).count();

    if (state.hasLast && weapon == state.lastWeapon && now - state.lastShot <= BURST_GAP)
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastShot).count();
        std::uint32_t minMs = static_cast<std::uint32_t>(ms);
        std::uint64_t avgMs = static_cast<std::uint64_t>(ms);
        if (weapon < state.stats.size())
        {
            WeaponStat &ws = state.stats[weapon];
            ++ws.count;
            ws.sumMs += static_cast<std::uint64_t>(ms);
            if (ws.minMs == 0 || ms < ws.minMs)
                ws.minMs = static_cast<std::uint32_t>(ms);
            minMs = ws.minMs;
            avgMs = ws.sumMs / ws.count;
        }
        m_gameTextService.show(player,
                               u(fmt::format("~y~ID {}  ~w~{} мс~n~мин {}  ср {}~n~~b~таблица {} мс", weapon, ms,
                                             minMs, avgMs, table)),
                               GT_TIME, GT_STYLE);
    }
    else
    {
        m_gameTextService.show(
            player, u(fmt::format("~y~ID {}  ~w~новая очередь~n~~b~таблица {} мс", weapon, table)), GT_TIME, GT_STYLE);
    }

    state.hasLast = true;
    state.lastWeapon = weapon;
    state.lastShot = now;
    return true; // замер никогда не вмешивается в конвейер выстрела
}

bool WeaponDebugSystem::onPlayerShotMissed(IPlayer &player, const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData);
}

bool WeaponDebugSystem::onPlayerShotPlayer(IPlayer &player, IPlayer &target, const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData);
}

bool WeaponDebugSystem::onPlayerShotVehicle(IPlayer &player, IVehicle &target, const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData);
}

bool WeaponDebugSystem::onPlayerShotObject(IPlayer &player, IObject &target, const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData);
}

bool WeaponDebugSystem::onPlayerShotPlayerObject(IPlayer &player, IPlayerObject &target,
                                                 const PlayerBulletData &bulletData)
{
    return handleShot(player, bulletData);
}

void WeaponDebugSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_state[player.getID()] = DebugState{};
}
