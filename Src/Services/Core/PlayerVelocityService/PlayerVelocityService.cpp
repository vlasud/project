#include "Services/Core/PlayerVelocityService/PlayerVelocityService.h"

#include <chrono>
#include <cmath>
#include <fmt/format.h>

namespace
{
// Лимиты «устойчивой» скорости (м/с). Разовые пики не караются — нарушение
// фиксируется, только если скорость держится над лимитом дольше OVER_DURATION.
// Спринт ~7 м/с; запас на прыжки, горки и рассинхрон даёт лимит, но не настолько
// широкий, чтобы под него влезал CLEO-ускоритель бега (обычно 1.5-2x спринта).
constexpr float FOOT_HORIZONTAL_MAX = 9.0f;
// Подъём пешком: прыжок даёт ~5 м/с, но всплеском. Устойчивый набор высоты — это
// уже airbreak/полёт: ни лестниц, ни лифтов такой скорости в игре нет (движущиеся
// платформы приходят сёрфингом и считаются по транспортному лимиту).
constexpr float FOOT_UP_MAX = 7.0f;
// В свободном падении горизонталь наследуется от транспорта (прыжок из Гидры
// на полном ходу), вертикаль вниз не ограничиваем вовсе.
constexpr float FALL_HORIZONTAL_MAX = 110.0f;
// Транспорт: самые быстрые самолёты ~75-80 м/с; лимит ловит 2x-спидхак.
constexpr float VEHICLE_MAX = 120.0f;

// Порог «игрок падает»: вертикальная скорость вниз быстрее этого значения.
constexpr float FALLING_VZ = -2.0f;

// Сколько скорость должна продержаться над лимитом, чтобы это было нарушением.
// Покрывает легальные всплески: выход из машины на ходу, отброс взрывом,
// скольжение после приземления. Окно шире прежнего — плата за сниженные лимиты:
// короткий легальный выброс скорости прощается, устойчивый чит нет.
constexpr std::chrono::milliseconds OVER_DURATION{2000};

// Quick turn: модель в игре разворачивается анимацией, мгновенно поменять угол
// может только чит. Одиночный «разворот» бывает и легальным (пакеты слиплись,
// клиент лагнул), поэтому нарушением считается СЕРИЯ в скользящем окне.
constexpr float SNAP_ANGLE = 170.0f;  // почти кругом
constexpr float SNAP_MAX_DT = 0.12f;  // за время одного-двух синков
constexpr std::uint8_t SNAP_LIMIT = 8; // столько разворотов в окне рукой не сделать
constexpr std::chrono::seconds SNAP_WINDOW{60};

// Разница углов (градусы) по кратчайшей дуге: [0, 180].
float angleDelta(float a, float b)
{
    const float diff = std::fmod(std::abs(a - b), 360.0f);
    return diff > 180.0f ? 360.0f - diff : diff;
}

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
        st.hasYaw = false;
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
        st.hasYaw = false; // угол после телепорта/спавна меняется скачком легально
        st.lastDiscontinuity = discontinuity;
        st.lastPosition = position;
        st.lastSample = now;
        st.velocity = {};
        st.overSince = {};
        return outcome;
    }

    // Реальный разрыв между синками: для скорости он клампится (анти-спидхак), а
    // развороту нужен именно фактический — за 0.5 с кругом повернуться законно.
    const float rawDt = std::chrono::duration<float>(now - st.lastSample).count();
    const float dt = clampDt(rawDt);
    const Vector3 raw = (position - st.lastPosition) / dt;
    st.lastPosition = position;
    st.lastSample = now;

    // EMA: новая скорость подмешивается, дрожание одного тика гасится.
    st.velocity = st.velocity * (1.0f - EMA_ALPHA) + raw * EMA_ALPHA;

    // --- мгновенные развороты модели (quick turn) ---
    // Только пешком: угол в транспорте ведёт машина, а на сёрфе — платформа.
    const float yaw = player.getRotation().ToEuler().z;
    if (playerState == PlayerState_OnFoot && std::isfinite(yaw))
    {
        if (st.hasYaw && angleDelta(yaw, st.lastYaw) >= SNAP_ANGLE && rawDt <= SNAP_MAX_DT)
        {
            if (st.snapCount == 0 || now - st.snapWindowStart > SNAP_WINDOW)
            {
                st.snapWindowStart = now;
                st.snapCount = 0;
            }
            ++st.snapCount;
            if (st.snapCount >= SNAP_LIMIT)
            {
                st.snapCount = 0;
                st.snapWindowStart = now;
                outcome.quickTurn = true;
                outcome.detail = fmt::format("{} instant turns within {}s", static_cast<int>(SNAP_LIMIT),
                                             static_cast<int>(SNAP_WINDOW.count()));
            }
        }
        st.lastYaw = yaw;
        st.hasYaw = true;
    }
    else
    {
        st.hasYaw = false;
    }

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
