#include "Services/Core/PlayerChatService/PlayerChatService.h"

#include <utility>

namespace
{
// Темп: burst BURST сообщений подряд, дальше одно на REFILL_INTERVAL.
constexpr float BURST = 3.0f;
constexpr std::chrono::milliseconds REFILL_INTERVAL{1500};

// Повтор одинакового текста в течение этого окна — спам.
constexpr std::chrono::milliseconds DUPLICATE_WINDOW{10000};

// Отказы антиспама: STRIKES_TO_MUTE за STRIKE_WINDOW → автомут на AUTO_MUTE.
constexpr int STRIKES_TO_MUTE = 3;
constexpr std::chrono::milliseconds STRIKE_WINDOW{30000};
constexpr std::chrono::seconds AUTO_MUTE{30};

int secondsLeft(TimePoint until, TimePoint now)
{
    if (until <= now)
        return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(until - now).count()) + 1;
}
} // namespace

void PlayerChatService::addStrike(State &st, TimePoint now)
{
    if (now - st.lastStrike > STRIKE_WINDOW)
        st.strikes = 0;
    st.lastStrike = now;
    ++st.strikes;

    if (st.strikes >= STRIKES_TO_MUTE)
    {
        st.strikes = 0;
        st.muteUntil = now + AUTO_MUTE;
    }
}

PlayerChatService::Check PlayerChatService::tryChat(int playerId, StringView message, TimePoint now)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return {Block::Muted, 0};

    State &st = m_state[playerId];

    if (st.muteUntil > now)
    {
        return {Block::Muted, secondsLeft(st.muteUntil, now)};
    }

    // Темп: токен-бакет, устойчивый к коротким очередям сообщений.
    if (st.lastRefill.time_since_epoch().count() != 0)
    {
        const float elapsedMs = std::chrono::duration<float, std::milli>(now - st.lastRefill).count();
        st.tokens += elapsedMs / std::chrono::duration<float, std::milli>(REFILL_INTERVAL).count();
        if (st.tokens > BURST)
            st.tokens = BURST;
    }
    st.lastRefill = now;
    if (st.tokens < 1.0f)
    {
        addStrike(st, now);
        if (st.muteUntil > now) // автомут только что сработал
            return {Block::Muted, secondsLeft(st.muteUntil, now)};
        return {Block::TooFast, 0};
    }

    // Повтор: тот же текст в коротком окне.
    if (!st.lastMessage.empty() && now - st.lastMessageAt <= DUPLICATE_WINDOW && message == StringView(st.lastMessage))
    {
        addStrike(st, now);
        if (st.muteUntil > now)
            return {Block::Muted, secondsLeft(st.muteUntil, now)};
        return {Block::Duplicate, 0};
    }

    // Принято: тратим токен, запоминаем текст.
    st.tokens -= 1.0f;
    st.lastMessage.assign(message.data(), message.size());
    st.lastMessageAt = now;
    return {Block::None, 0};
}

void PlayerChatService::subscribeSpeech(SpeechObserver observer)
{
    if (observer)
        m_speechObservers.push_back(std::move(observer));
}

bool PlayerChatService::notifySpeech(int playerId, StringView message) const
{
    if (!validPlayerId(playerId))
        return false;
    for (const SpeechObserver &observer : m_speechObservers)
    {
        // Первый забравший реплику останавливает обход: показать её дважды разными
        // наблюдателями хуже, чем отдать одному.
        if (observer(playerId, message))
            return true;
    }
    return false;
}

void PlayerChatService::mute(int playerId, std::chrono::seconds duration)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_state[playerId].muteUntil = std::chrono::steady_clock::now() + duration;
}

void PlayerChatService::unmute(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_state[playerId].muteUntil = {};
}

bool PlayerChatService::isMuted(int playerId) const
{
    return muteSecondsLeft(playerId) > 0;
}

int PlayerChatService::muteSecondsLeft(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    return secondsLeft(m_state[playerId].muteUntil, std::chrono::steady_clock::now());
}

void PlayerChatService::reset(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    m_state[playerId] = State{};
}
