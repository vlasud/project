#include "Services/Core/WorldService/WorldService.h"

#include "core.hpp"
#include <algorithm>

// ------------------------------------------------------------------ время

void WorldService::setTime(int hour, int minute)
{
    m_hour = std::clamp(hour, 0, 23);
    m_minute = std::clamp(minute, 0, 59);

    if (m_core)
    {
        // Час попадает в InitGame — новые подключения получат его сразу;
        // точные минуты доедут на спавне и при тиках течения времени.
        m_core->setWorldTime(Hours(m_hour));
    }
    broadcastTime();
}

void WorldService::getTime(int &hour, int &minute) const
{
    hour = m_hour;
    minute = m_minute;
}

void WorldService::setTimeFlowing(bool flowing, Milliseconds realPerGameMinute)
{
    m_flowing = flowing;
    m_perGameMinute = std::max(realPerGameMinute, Milliseconds(50));
    rescheduleClock();
}

bool WorldService::isTimeFlowing() const
{
    return m_flowing;
}

// ------------------------------------------------------------------ погода

void WorldService::setWeather(int weatherId)
{
    m_weather = std::clamp(weatherId, 0, MAX_WEATHER_ID);
    if (!m_core)
    {
        return;
    }

    // Глобальный сеттер обновляет InitGame для новых и рассылает всем; затем
    // возвращаем персональные оверрайды тем, у кого они есть.
    m_core->setWeather(m_weather);
    for (IPlayer *player : m_core->getPlayers().entries())
    {
        if (m_weatherOverride[player->getID()])
        {
            player->setWeather(m_overrideWeather[player->getID()]);
        }
    }
}

int WorldService::getWeather() const
{
    return m_weather;
}

void WorldService::setWeatherForPlayer(IPlayer &player, int weatherId)
{
    const int playerId = player.getID();
    m_weatherOverride[playerId] = true;
    m_overrideWeather[playerId] = std::clamp(weatherId, 0, MAX_WEATHER_ID);
    player.setWeather(m_overrideWeather[playerId]);
}

void WorldService::clearWeatherForPlayer(IPlayer &player)
{
    const int playerId = player.getID();
    if (!m_weatherOverride[playerId])
    {
        return;
    }
    m_weatherOverride[playerId] = false;
    player.setWeather(m_weather);
}

// ------------------------------------------------------------------ часы

void WorldService::showClock(bool visible)
{
    m_clockVisible = visible;
    if (!m_core)
    {
        return;
    }
    for (IPlayer *player : m_core->getPlayers().entries())
    {
        player->useClock(visible);
    }
}

bool WorldService::isClockVisible() const
{
    return m_clockVisible;
}

// ------------------------------------------------------------------ вызовы WorldSystem

void WorldService::initialize(ICore *core, TimerService *timers)
{
    m_core = core;
    m_timers = timers;
}

void WorldService::handleSpawn(IPlayer &player)
{
    // Полная синхронизация состояния мира заспавнившемуся (включая минуты,
    // которых нет в InitGame).
    player.setTime(Hours(m_hour), Minutes(m_minute));
    player.useClock(m_clockVisible);
    player.setWeather(m_weatherOverride[player.getID()] ? m_overrideWeather[player.getID()] : m_weather);
}

void WorldService::resetPlayer(int playerId)
{
    m_weatherOverride[playerId] = false;
}

// ------------------------------------------------------------------ private

void WorldService::advanceMinute()
{
    if (++m_minute >= 60)
    {
        m_minute = 0;
        m_hour = (m_hour + 1) % 24;
        if (m_core)
        {
            m_core->setWorldTime(Hours(m_hour)); // держим InitGame в актуальном часе
        }
    }
    broadcastTime();
}

void WorldService::rescheduleClock()
{
    if (!m_timers)
    {
        return;
    }
    m_timers->cancel(m_clockTimer);
    if (m_flowing)
    {
        m_clockTimer = m_timers->setInterval(m_perGameMinute, [this] { advanceMinute(); });
    }
}

void WorldService::broadcastTime()
{
    if (!m_core)
    {
        return;
    }
    for (IPlayer *player : m_core->getPlayers().entries())
    {
        player->setTime(Hours(m_hour), Minutes(m_minute));
    }
}
