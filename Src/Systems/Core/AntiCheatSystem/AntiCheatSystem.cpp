#include "Systems/Core/AntiCheatSystem/AntiCheatSystem.h"

#include "Log/LogManager.h"
#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <string>

namespace
{
// Политика: кик при KICK_THRESHOLD нарушениях за скользящее окно KICK_WINDOW.
// Одиночные записи (лаг, пограничный случай) кика не вызывают — активный чит
// генерирует нарушения непрерывно и добирает порог за секунды.
constexpr std::size_t KICK_THRESHOLD = 5;
constexpr std::chrono::seconds KICK_WINDOW{60};

const char *clientVersionName(ClientVersion version)
{
    switch (version)
    {
    case ClientVersion::ClientVersion_SAMP_037:
        return "SA-MP 0.3.7";
    case ClientVersion::ClientVersion_SAMP_03DL:
        return "SA-MP 0.3.DL";
    case ClientVersion::ClientVersion_openmp:
        return "open.mp";
    }
    return "unknown";
}
} // namespace

AntiCheatSystem::AntiCheatSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_antiCheatService(serviceRegister.getService<AntiCheatService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    m_antiCheatService.subscribe(
        [this](int playerId, AntiCheatService::ViolationType type, const AntiCheatService::PlayerRecord &record)
        { onViolation(playerId, type, record); });
}

void AntiCheatSystem::onViolation(int playerId, AntiCheatService::ViolationType type,
                                  const AntiCheatService::PlayerRecord &record)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }

    LogManager::log(LogLevel::Warning, fmt::format("[AntiCheat] {} (id {}): {} — {}", player->getName(), playerId,
                                                   AntiCheatService::name(type), record.recent.back().detail));

    const TimePoint windowStart = record.lastAt - KICK_WINDOW;
    const std::size_t recentCount =
        std::count_if(record.recent.begin(), record.recent.end(),
                      [windowStart](const AntiCheatService::Violation &v) { return v.time >= windowStart; });

    if (recentCount >= KICK_THRESHOLD)
    {
        LogManager::log(LogLevel::Warning,
                        fmt::format("[AntiCheat] Kicking {} (id {}): {} violations in window, {} total",
                                    player->getName(), playerId, recentCount, record.total));
        player->kick();
    }
}

void AntiCheatSystem::onPlayerConnect(IPlayer &player)
{
    // Версию заявляет клиент, доверять ей нельзя — это телеметрия, не проверка:
    // по ней видно, с каких сборок идут нарушения (0.3.7 — база большинства читов).
    const StringView build = player.getClientVersionName();
    LogManager::log(LogLevel::Message,
                    fmt::format("[AntiCheat] {} (id {}) connected: client {} (build \"{}\")", player.getName(),
                                player.getID(), clientVersionName(player.getClientVersion()),
                                std::string(build.data(), build.size())));
}

void AntiCheatSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_antiCheatService.clear(player.getID());
}
