#include "Services/Core/GameTextService/GameTextService.h"

#include "core.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
constexpr Milliseconds TIME_MIN{100};
constexpr Milliseconds TIME_MAX{600000}; // дольше 10 минут — это уже textdraw
} // namespace

std::string GameTextService::sanitizeText(StringView text)
{
    std::string result;
    result.reserve(std::min(text.size(), MAX_TEXT_LENGTH));

    for (char c : text)
    {
        if (result.size() >= MAX_TEXT_LENGTH)
        {
            break;
        }
        if (static_cast<unsigned char>(c) >= 0x20)
        {
            result += c;
        }
    }

    // Непарная '~' ломает разбор кодов на клиенте — убираем последнюю.
    if (std::count(result.begin(), result.end(), '~') % 2 != 0)
    {
        result.erase(result.rfind('~'), 1);
    }

    return result; // пустой результат вызывающие методы не отправляют
}

void GameTextService::show(IPlayer &player, StringView text, Milliseconds time, int style)
{
    const std::string safe = sanitizeText(text);
    if (safe.empty())
    {
        return;
    }
    player.sendGameText(safe, clampTime(time), clampStyle(style));
}

void GameTextService::showToAll(StringView text, Milliseconds time, int style)
{
    if (!m_core)
    {
        return;
    }
    const std::string safe = sanitizeText(text);
    if (safe.empty())
    {
        return;
    }
    m_core->getPlayers().sendGameTextToAll(safe, clampTime(time), clampStyle(style));
}

void GameTextService::showNear(const Vector3 &position, float radius, StringView text, Milliseconds time, int style)
{
    if (!m_core || !std::isfinite(radius) || radius <= 0.0f)
    {
        return;
    }
    const std::string safe = sanitizeText(text);
    if (safe.empty())
    {
        return;
    }

    const Milliseconds clampedTime = clampTime(time);
    const int clampedStyle = clampStyle(style);
    const float radiusSq = radius * radius;
    for (IPlayer *player : m_core->getPlayers().entries())
    {
        const Vector3 delta = player->getPosition() - position;
        if (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z <= radiusSq)
        {
            player->sendGameText(safe, clampedTime, clampedStyle);
        }
    }
}

void GameTextService::hide(IPlayer &player, int style)
{
    player.hideGameText(clampStyle(style));
}

void GameTextService::hideForAll(int style)
{
    if (m_core)
    {
        m_core->getPlayers().hideGameTextForAll(clampStyle(style));
    }
}

bool GameTextService::isVisible(IPlayer &player, int style) const
{
    return player.hasGameText(clampStyle(style));
}

// ------------------------------------------------------------------ private

void GameTextService::initialize(ICore *core)
{
    m_core = core;
}

int GameTextService::clampStyle(int style)
{
    return std::clamp(style, 0, MAX_STYLE);
}

Milliseconds GameTextService::clampTime(Milliseconds time)
{
    return std::clamp(time, TIME_MIN, TIME_MAX);
}
