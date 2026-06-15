#include "Services/ReportService/ReportService.h"

#include "Log/LogManager.h"
#include <fmt/format.h>

bool ReportService::ready(int playerId, TimePoint now) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    const State &state = m_state[playerId];
    // Первый репорт всегда проходит; иначе — только когда окно кулдауна истекло.
    return !state.used || now - state.lastReportAt >= REPORT_COOLDOWN;
}

int ReportService::secondsLeft(int playerId, TimePoint now) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    const State &state = m_state[playerId];
    if (!state.used)
        return 0;
    const auto elapsed = now - state.lastReportAt;
    if (elapsed >= REPORT_COOLDOWN)
        return 0;
    // ceil остатка в секундах (как сообщение об остатке блока команд).
    const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(REPORT_COOLDOWN - elapsed);
    return static_cast<int>((remainingMs.count() + 999) / 1000);
}

void ReportService::record(int playerId, TimePoint now, const std::string &playerName, const std::string &text)
{
    if (playerId >= 0 && playerId < MAX_PLAYERS)
    {
        State &state = m_state[playerId];
        state.lastReportAt = now;
        state.used = true;
    }
    // text — utf-8 (логи и БД в utf-8); рассылку игрокам делает вызывающий.
    LogManager::log(LogLevel::Message, fmt::format("[REPORT] {}[{}]: {}", playerName, playerId, text));
}

void ReportService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_state[playerId] = State{};
}
