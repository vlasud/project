#include "Services/Core/AudioService/AudioService.h"

#include "core.hpp"
#include <algorithm>
#include <cmath>

namespace
{
// Валидных ID звуков SA меньше; неизвестные клиент просто не играет, но
// мусорные значения наружу не шлём.
constexpr std::uint32_t MAX_SOUND_ID = 60000;
constexpr float MAX_STREAM_DISTANCE = 500.0f;

bool startsWithNoCase(StringView text, StringView prefix)
{
    if (text.size() < prefix.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i)
    {
        const char a = text[i];
        const char b = prefix[i];
        const char lowerA = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
        if (lowerA != b)
        {
            return false;
        }
    }
    return true;
}
} // namespace

bool AudioService::validateStreamUrl(StringView url, std::string &error)
{
    if (url.empty())
    {
        error = "пустой URL";
        return false;
    }
    if (url.size() > MAX_URL_LENGTH)
    {
        error = "URL длиннее " + std::to_string(MAX_URL_LENGTH) + " символов";
        return false;
    }
    if (!startsWithNoCase(url, "http://") && !startsWithNoCase(url, "https://"))
    {
        error = "URL должен начинаться с http:// или https://";
        return false;
    }
    for (char c : url)
    {
        // Только печатный ASCII без пробелов: клиентский парсер URL другого не ждёт.
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) > 0x7E)
        {
            error = "URL содержит недопустимые символы";
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ звуки

void AudioService::playSound(IPlayer &player, std::uint32_t soundId)
{
    // Позиция (0,0,0) — конвенция клиента «не позиционный, на полную громкость».
    player.playSound(clampSoundId(soundId), Vector3(0.0f, 0.0f, 0.0f));
}

void AudioService::playSoundAt(IPlayer &player, std::uint32_t soundId, const Vector3 &position)
{
    player.playSound(clampSoundId(soundId), position);
}

void AudioService::playSoundNear(const Vector3 &position, float radius, std::uint32_t soundId)
{
    if (!m_core || !std::isfinite(radius) || radius <= 0.0f)
    {
        return;
    }

    const std::uint32_t sound = clampSoundId(soundId);
    const float radiusSq = radius * radius;
    for (IPlayer *player : m_core->getPlayers().entries())
    {
        const Vector3 delta = player->getPosition() - position;
        if (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z <= radiusSq)
        {
            player->playSound(sound, position);
        }
    }
}

// ------------------------------------------------------------------ потоки

bool AudioService::playStream(IPlayer &player, StringView url)
{
    std::string error;
    if (!validateStreamUrl(url, error))
    {
        return false;
    }
    m_streams[player.getID()] = url.to_string();
    player.playAudio(url);
    return true;
}

bool AudioService::playStreamAt(IPlayer &player, StringView url, const Vector3 &position, float distance)
{
    std::string error;
    if (!validateStreamUrl(url, error))
    {
        return false;
    }
    if (!std::isfinite(distance))
    {
        distance = 30.0f;
    }
    distance = std::clamp(distance, 1.0f, MAX_STREAM_DISTANCE);

    m_streams[player.getID()] = url.to_string();
    player.playAudio(url, true, position, distance);
    return true;
}

void AudioService::stopStream(IPlayer &player)
{
    m_streams[player.getID()].clear();
    player.stopAudio();
}

bool AudioService::isStreaming(int playerId) const
{
    if (!validPlayerId(playerId))
        return false;
    return !m_streams[playerId].empty();
}

const std::string &AudioService::currentStream(int playerId) const
{
    return m_streams[playerId];
}

// ------------------------------------------------------------------ private

void AudioService::initialize(ICore *core)
{
    m_core = core;
}

void AudioService::resetPlayer(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    m_streams[playerId].clear();
}

std::uint32_t AudioService::clampSoundId(std::uint32_t soundId)
{
    return std::min(soundId, MAX_SOUND_ID);
}
