#include "Services/Core/WorldService/WorldService.h"

#include "core.hpp"
#include <algorithm>
#include <ctime>

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
    if (flowing)
    {
        m_realTime = false; // взаимоисключение: один писатель времени
    }
    m_perGameMinute = std::max(realPerGameMinute, Milliseconds(50));
    rescheduleClock();
}

bool WorldService::isTimeFlowing() const
{
    return m_flowing;
}

void WorldService::setRealTimeSync(bool enable)
{
    m_realTime = enable;
    if (enable)
    {
        m_flowing = false; // взаимоисключение: один писатель времени
    }
    rescheduleClock();
}

bool WorldService::isRealTimeSynced() const
{
    return m_realTime;
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

// ------------------------------------------------------------------ флаги InitGame

void WorldService::setStuntBonuses(bool enable)
{
    if (!m_core)
    {
        return;
    }
    // Конфиг — для InitGame новых подключений, useStuntBonuses — рассылка текущим.
    if (bool *value = m_core->getConfig().getBool("game.use_stunt_bonuses"))
    {
        *value = enable;
    }
    m_core->useStuntBonuses(enable);
}

bool WorldService::stuntBonusesEnabled() const
{
    if (!m_core)
    {
        return true; // дефолт ядра
    }
    const bool *value = const_cast<ICore *>(m_core)->getConfig().getBool("game.use_stunt_bonuses");
    return value ? *value : true;
}

void WorldService::setInteriorEnterExits(bool enable)
{
    if (!m_core)
    {
        return;
    }
    if (bool *value = m_core->getConfig().getBool("game.use_entry_exit_markers"))
    {
        *value = enable;
    }
}

bool WorldService::interiorEnterExitsEnabled() const
{
    if (!m_core)
    {
        return true; // дефолт ядра
    }
    const bool *value = const_cast<ICore *>(m_core)->getConfig().getBool("game.use_entry_exit_markers");
    return value ? *value : true;
}

void WorldService::setNameTags(bool show)
{
    if (!m_core)
    {
        return;
    }
    if (bool *value = m_core->getConfig().getBool("game.use_nametags"))
    {
        *value = show;
    }
}

bool WorldService::nameTagsEnabled() const
{
    if (!m_core)
    {
        return true;
    }
    const bool *value = const_cast<ICore *>(m_core)->getConfig().getBool("game.use_nametags");
    return value ? *value : true;
}

void WorldService::setPlayerMarkerMode(PlayerMarkerMode mode)
{
    if (!m_core)
    {
        return;
    }
    if (mode < PlayerMarkerMode_Off || mode > PlayerMarkerMode_Streamed)
    {
        mode = PlayerMarkerMode_Global;
    }
    if (int *value = m_core->getConfig().getInt("game.player_marker_mode"))
    {
        *value = static_cast<int>(mode);
    }
}

PlayerMarkerMode WorldService::playerMarkerMode() const
{
    if (!m_core)
    {
        return PlayerMarkerMode_Global;
    }
    const int *value = const_cast<ICore *>(m_core)->getConfig().getInt("game.player_marker_mode");
    if (!value || *value < PlayerMarkerMode_Off || *value > PlayerMarkerMode_Streamed)
    {
        return PlayerMarkerMode_Global;
    }
    return static_cast<PlayerMarkerMode>(*value);
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

void WorldService::applyRealTime()
{
    // Локальное время машины сервера; localtime — только главный поток (static-
    // буфер), значения копируются сразу. Перевод системных часов/DST не требует
    // обработки: следующий тик прочитает актуальное время и просто перескочит на
    // него (скачок освещения на час при DST — приемлем).
    int hour = m_hour;
    int minute = m_minute;
    int second = 0;
    const std::time_t now = std::time(nullptr);
    if (const std::tm *local = std::localtime(&now))
    {
        hour = std::clamp(local->tm_hour, 0, 23);
        minute = std::clamp(local->tm_min, 0, 59);
        second = local->tm_sec;
    }

    // Как advanceMinute: InitGame-час ядру — только при СМЕНЕ часа (setWorldTime
    // сам рассылает свой RPC всем — каждую минуту он был бы дублем broadcastTime).
    if (hour != m_hour && m_core)
    {
        m_core->setWorldTime(Hours(hour));
    }
    m_hour = hour;
    m_minute = minute;
    broadcastTime();

    if (m_timers)
    {
        // Перепланируем себя на границу следующей реальной минуты (+250мс зазора,
        // чтобы не проснуться на 59-й секунде той же минуты из-за огрубления
        // таймера; leap second tm_sec==60 гасится клампом). Перезапись своего же
        // хэндла из колбэка безопасна (контракт TimerService).
        const int untilNextMinute = 60 - std::clamp(second, 0, 59);
        m_clockTimer = m_timers->setTimeout(Milliseconds(untilNextMinute * 1000 + 250), [this] { applyRealTime(); });
    }
}

void WorldService::rescheduleClock()
{
    if (!m_timers)
    {
        return;
    }
    m_timers->cancel(m_clockTimer);
    if (m_realTime)
    {
        applyRealTime(); // применить сразу + самопланирование по границам минут
    }
    else if (m_flowing)
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
