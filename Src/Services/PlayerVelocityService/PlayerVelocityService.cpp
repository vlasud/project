#include "PlayerVelocityService.h"

#include <chrono>
#include <cmath>
#include <fmt/format.h>

namespace
{
// Лимиты «устойчивой» скорости (м/с). Разовые пики не караются — нарушение
// фиксируется, только если скорость держится над лимитом дольше OVER_DURATION.
// Спринт ~7 м/с, лимит с запасом на прыжки, горки и рассинхрон.
constexpr float FOOT_HORIZONTAL_MAX = 12.0f;
// Подъём пешком: прыжок ~5 м/с. Устойчивый подъём быстрее — airbreak вверх.
constexpr float FOOT_UP_MAX = 10.0f;
// В свободном падении горизонталь наследуется от транспорта (прыжок из Гидры
// на полном ходу), вертикаль вниз не ограничиваем вовсе.
constexpr float FALL_HORIZONTAL_MAX = 110.0f;
// Транспорт: самые быстрые самолёты ~75-80 м/с; лимит ловит 2x-спидхак.
constexpr float VEHICLE_MAX = 120.0f;

// Порог «игрок падает»: вертикальная скорость вниз быстрее этого значения.
constexpr float FALLING_VZ = -2.0f;

// Сколько скорость должна продержаться над лимитом, чтобы это было нарушением.
// Покрывает легальные всплески: выход из машины на ходу, отброс взрывом,
// скольжение после приземления.
constexpr std::chrono::milliseconds OVER_DURATION{1500};

// Сглаживание EMA: гасит дрожание производной по дискретным sync-позициям.
constexpr float EMA_ALPHA = 0.5f;

float clampDt(float seconds)
{
    if (seconds < 0.01f)
        return 0.01f;
    if (seconds > 1.0f)
        return 1.0f;
    return seconds;
}
} // namespace

void PlayerVelocityService::bind(PlayerLocationService &location)
{
    m_location = &location;
}

Vector3 PlayerVelocityService::getVelocity(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return {};
    return m_state[playerId].velocity;
}

float PlayerVelocityService::getSpeed(int playerId) const
{
    const Vector3 v = getVelocity(playerId);
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float PlayerVelocityService::getHorizontalSpeed(int playerId) const
{
    const Vector3 v = getVelocity(playerId);
    return std::sqrt(v.x * v.x + v.y * v.y);
}

float PlayerVelocityService::getVerticalSpeed(int playerId) const
{
    return getVelocity(playerId).z;
}

PlayerVelocityService::VerifyOutcome PlayerVelocityService::sample(IPlayer &player, TimePoint now)
{
    VerifyOutcome outcome;
    if (!m_location)
        return outcome;

    const int playerId = player.getID();
    State &st = m_state[playerId];

    const PlayerState playerState = player.getState();
    const bool playing = playerState == PlayerState_OnFoot || playerState == PlayerState_Driver ||
                         playerState == PlayerState_Passenger;
    if (!playing || m_location->isBypassed(playerId))
    {
        st.hasSample = false;
        st.velocity = {};
        st.overSince = {};
        return outcome;
    }

    const Vector3 position = m_location->getPosition(playerId);
    const std::uint32_t discontinuity = m_location->getDiscontinuity(playerId);

    // Первый сэмпл или разрыв непрерывности (телепорт/спавн/грейс/пауза) —
    // производную не считаем, только перезаряжаем точку отсчёта.
    if (!st.hasSample || discontinuity != st.lastDiscontinuity)
    {
        st.hasSample = true;
        st.lastDiscontinuity = discontinuity;
        st.lastPosition = position;
        st.lastSample = now;
        st.velocity = {};
        st.overSince = {};
        return outcome;
    }

    const float dt = clampDt(std::chrono::duration<float>(now - st.lastSample).count());
    const Vector3 raw = (position - st.lastPosition) / dt;
    st.lastPosition = position;
    st.lastSample = now;

    // EMA: новая скорость подмешивается, дрожание одного тика гасится.
    st.velocity = st.velocity * (1.0f - EMA_ALPHA) + raw * EMA_ALPHA;

    // --- валидация устойчивого превышения ---
    const float horizontal = std::sqrt(st.velocity.x * st.velocity.x + st.velocity.y * st.velocity.y);
    const float vertical = st.velocity.z;

    const bool inVehicle = playerState == PlayerState_Driver || playerState == PlayerState_Passenger;
    const bool surfing = player.getSurfingData().type != PlayerSurfingData::Type::None;
    const bool falling = vertical < FALLING_VZ;

    bool over = false;
    const char *what = "";
    float value = 0.0f;
    float limit = 0.0f;

    if (inVehicle || surfing)
    {
        // Транспорт и сёрф: единый щедрый лимит полной скорости.
        const float speed = std::sqrt(horizontal * horizontal + vertical * vertical);
        if (speed > VEHICLE_MAX)
        {
            over = true;
            what = "vehicle speed";
            value = speed;
            limit = VEHICLE_MAX;
        }
    }
    else if (falling)
    {
        // Падение: вертикаль вниз свободна, горизонталь — от прыжка из самолёта.
        if (horizontal > FALL_HORIZONTAL_MAX)
        {
            over = true;
            what = "falling horizontal speed";
            value = horizontal;
            limit = FALL_HORIZONTAL_MAX;
        }
    }
    else
    {
        // Пешком на земле: и горизонталь, и устойчивый подъём (airbreak).
        if (horizontal > FOOT_HORIZONTAL_MAX)
        {
            over = true;
            what = "onfoot horizontal speed";
            value = horizontal;
            limit = FOOT_HORIZONTAL_MAX;
        }
        else if (vertical > FOOT_UP_MAX)
        {
            over = true;
            what = "onfoot climb speed";
            value = vertical;
            limit = FOOT_UP_MAX;
        }
    }

    if (!over)
    {
        st.overSince = {};
        return outcome;
    }

    if (st.overSince.time_since_epoch().count() == 0)
    {
        st.overSince = now; // первый тик над лимитом — ждём, не всплеск ли
        return outcome;
    }

    if (now - st.overSince >= OVER_DURATION)
    {
        st.overSince = now; // не зачитывать одно превышение каждый апдейт
        outcome.speedHack = true;
        outcome.detail = fmt::format("{} {:.1f} m/s (max {:.1f}) sustained", what, value, limit);
    }

    return outcome;
}

void PlayerVelocityService::reset(int playerId)
{
    m_state[playerId] = State{};
}
