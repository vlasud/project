#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"

#include <chrono>
#include <fmt/format.h>

namespace
{
// Окно на синхронизацию: после применения анимации клиенту нужно несколько
// сетевых тиков, чтобы получить RPC, проиграть анимацию и отправить её обратно в
// синхронизации. До истечения этого окна несовпадение — это ещё не нарушение.
constexpr std::chrono::milliseconds SYNC_GRACE{1200};

inline char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// Имена анимаций в SDK хранятся в верхнем регистре, а вызывающий код может задать
// любой — поэтому сравниваем без учёта регистра.
bool iequals(StringView a, StringView b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (asciiLower(a[i]) != asciiLower(b[i]))
            return false;
    return true;
}

bool sameAnim(const AnimationData &data, StringView lib, StringView name)
{
    return iequals(StringView(data.lib), lib) && iequals(StringView(data.name), name);
}
} // namespace

void PlayerAnimationService::play(IPlayer &player, const AnimationData &animation, bool interruptible)
{
    State &state = m_state[player.getID()];
    state.active = true;
    state.interruptible = interruptible;
    state.confirmed = false;
    state.data = animation;
    state.appliedAt = std::chrono::steady_clock::now();

    player.applyAnimation(animation, PlayerAnimationSyncType_Sync);
}

void PlayerAnimationService::stop(IPlayer &player)
{
    State &state = m_state[player.getID()];
    state.active = false;
    state.confirmed = false;

    player.clearAnimations(PlayerAnimationSyncType_Sync);
}

bool PlayerAnimationService::isPlaying(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;

    const State &state = m_state[playerId];
    return state.active && state.confirmed;
}

bool PlayerAnimationService::isPlaying(int playerId, StringView lib, StringView name) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;

    const State &state = m_state[playerId];
    return state.active && state.confirmed && sameAnim(state.data, lib, name);
}

PlayerAnimationService::VerifyOutcome PlayerAnimationService::verify(IPlayer &player, TimePoint now)
{
    VerifyOutcome outcome;

    State &state = m_state[player.getID()];
    if (!state.active)
        return outcome;

    // Что клиент сообщает о своей текущей анимации.
    const PlayerAnimationData current = player.getAnimationData();
    const auto names = splitAnimationNames(current.ID);
    const bool matches = sameAnim(state.data, names.first, names.second);

    if (matches)
    {
        state.confirmed = true;
        return outcome;
    }

    // Ещё не подтверждено и окно синхронизации не вышло — ждём, это нормально.
    if (!state.confirmed && (now - state.appliedAt) < SYNC_GRACE)
        return outcome;

    if (state.interruptible)
    {
        // Прерываемая анимация завершилась или сменилась — это легально.
        state.active = false;
        state.confirmed = false;
        return outcome;
    }

    // Непрерываемая: клиент вышел из анимации (сам или читом) — переустанавливаем.
    // Сбрасываем окно синхронизации, чтобы не переустанавливать каждый апдейт,
    // пока клиент догоняет.
    player.applyAnimation(state.data, PlayerAnimationSyncType_Sync);
    state.appliedAt = now;
    state.confirmed = false;

    // Серверно-авторитетное нарушение: сервер заблокировал анимацию, а клиент в
    // ней не находится. Деталь — что требовалось против того, что заявил клиент.
    outcome.forcedAnimationEscaped = true;
    // fmt форматирует StringView напрямую (детектит как string-like); HybridString
    // оборачиваем в StringView явно, т.к. пользовательскую конверсию fmt не применяет.
    outcome.detail = fmt::format("forced {}:{}, reported {}:{}", StringView(state.data.lib),
                                 StringView(state.data.name), names.first, names.second);
    return outcome;
}

void PlayerAnimationService::reset(int playerId)
{
    m_state[playerId] = State{};
}
