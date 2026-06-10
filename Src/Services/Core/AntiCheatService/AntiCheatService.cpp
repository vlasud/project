#include "Services/Core/AntiCheatService/AntiCheatService.h"

void AntiCheatService::record(int playerId, ViolationType type, std::string detail, TimePoint now)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;

    PlayerRecord &record = m_records[playerId];
    if (record.total == 0)
        record.firstAt = now;
    ++record.total;
    record.lastAt = now;

    // Храним только последние RECENT_LIMIT записей с деталями: спамящий читер не
    // должен раздувать память без предела. Полный счётчик total при этом не теряется.
    record.recent.push_back({type, now, std::move(detail)});
    if (record.recent.size() > RECENT_LIMIT)
        record.recent.erase(record.recent.begin());

    for (const Observer &observer : m_observers)
        observer(playerId, type, record);
}

void AntiCheatService::subscribe(Observer observer)
{
    m_observers.push_back(std::move(observer));
}

const AntiCheatService::PlayerRecord &AntiCheatService::get(int playerId) const
{
    static const PlayerRecord empty;
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return empty;
    return m_records[playerId];
}

std::uint32_t AntiCheatService::count(int playerId) const
{
    return get(playerId).total;
}

bool AntiCheatService::flagged(int playerId) const
{
    return count(playerId) > 0;
}

void AntiCheatService::clear(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_records[playerId] = PlayerRecord{};
}
