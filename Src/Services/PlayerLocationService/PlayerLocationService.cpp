#include "PlayerLocationService.h"

#include "glm/geometric.hpp"
#include <chrono>
#include <fmt/format.h>

namespace
{
// Лимиты скорости сознательно ЩЕДРЫЕ: цель — ловить телепорты (сотни метров за
// тик), а не микро-спидхаки. Пеший лимит покрывает падение, отбросы и сёрф на
// транспорте; машинный — самые быстрые самолёты.
constexpr float FOOT_MAX_SPEED = 50.0f;     // м/с
constexpr float VEHICLE_MAX_SPEED = 150.0f; // м/с
constexpr float DIST_SLACK = 10.0f;         // метры поверх скоростного лимита

// Дальше этих границ легального мира не бывает — мгновенный откат.
constexpr float WORLD_BOUND_XY = 20000.0f;
constexpr float WORLD_MIN_Z = -1000.0f;
constexpr float WORLD_MAX_Z = 5000.0f;

// Разрыв апдейтов, после которого считаем что была пауза клиента: dt не копится,
// следующее заявление принимается без проверки скорости (игрок на паузе не движется).
constexpr std::chrono::milliseconds PAUSE_GAP{2000};

// Сколько ждём прибытия клиента в точку серверного телепорта (загрузка интерьера
// может занять секунды; пауза в апдейтах при загрузке сама продлевает ожидание).
constexpr std::chrono::milliseconds TELEPORT_GRACE{4000};

// Радиус прибытия: не больше этого, и не больше 40% дальности прыжка — иначе при
// коротком откате (10-20 м) запоздавшие пакеты с читерской позицией сами попадают
// в радиус и засчитываются как «прибыл», порождая каскад ложных нарушений.
constexpr float ARRIVE_MAX = 30.0f;
constexpr float ARRIVE_MIN = 5.0f;
constexpr float ARRIVE_FRACTION = 0.4f;

// Пешком после паузы позиция не должна была измениться (на паузе клиент заморожен).
// Допуск щедрый — на рассинхрон и редкие толчки. В транспорте лимита нет:
// пассажира на паузе машина легально увозит куда угодно.
constexpr float PAUSE_MOVE_TOLERANCE = 100.0f;

// Минимальный интервал между грейсами за смену интерьера: чит, спамящий
// interior-change ради легализации телепортов, упрётся в лимит.
constexpr std::chrono::milliseconds INTERIOR_GRACE_COOLDOWN{2000};

float clampDt(float seconds)
{
    if (seconds < 0.01f)
        return 0.01f;
    if (seconds > 1.0f)
        return 1.0f;
    return seconds;
}

bool inWorldBounds(const Vector3 &p)
{
    return p.x >= -WORLD_BOUND_XY && p.x <= WORLD_BOUND_XY && p.y >= -WORLD_BOUND_XY && p.y <= WORLD_BOUND_XY &&
           p.z >= WORLD_MIN_Z && p.z <= WORLD_MAX_Z;
}

bool isPlayingState(PlayerState state)
{
    return state == PlayerState_OnFoot || state == PlayerState_Driver || state == PlayerState_Passenger;
}
} // namespace

void PlayerLocationService::forceTo(IPlayer &player, const Vector3 &position, TimePoint now)
{
    State &st = m_state[player.getID()];

    // Радиус прибытия — от дальности прыжка: короткий откат требует точного
    // прибытия, дальний телепорт даёт запас на рельеф/падение в точке.
    const float jump = glm::distance(st.position, position);
    float radius = jump * ARRIVE_FRACTION;
    if (radius < ARRIVE_MIN)
        radius = ARRIVE_MIN;
    if (radius > ARRIVE_MAX)
        radius = ARRIVE_MAX;
    st.arriveRadius = radius;

    st.position = position;
    st.pendingTeleport = true;
    st.teleportTarget = position;
    st.teleportAt = now;
    ++st.discontinuity;
    player.setPosition(position);
}

void PlayerLocationService::teleport(IPlayer &player, const Vector3 &position)
{
    forceTo(player, position, std::chrono::steady_clock::now());
}

void PlayerLocationService::teleport(IPlayer &player, const Vector3 &position, unsigned interior, int virtualWorld)
{
    setInterior(player, interior);
    setVirtualWorld(player, virtualWorld);
    teleport(player, position);
}

void PlayerLocationService::setInterior(IPlayer &player, unsigned interior)
{
    m_state[player.getID()].interior = interior;
    player.setInterior(interior);
}

void PlayerLocationService::setVirtualWorld(IPlayer &player, int virtualWorld)
{
    m_state[player.getID()].virtualWorld = virtualWorld;
    player.setVirtualWorld(virtualWorld);
}

Vector3 PlayerLocationService::getPosition(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return {};
    return m_state[playerId].position;
}

unsigned PlayerLocationService::getInterior(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    return m_state[playerId].interior;
}

int PlayerLocationService::getVirtualWorld(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    return m_state[playerId].virtualWorld;
}

void PlayerLocationService::setBypass(int playerId, bool bypass)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_state[playerId].bypass = bypass;
    // После байпаса позиция могла уехать куда угодно — принимаем заново.
    m_state[playerId].acceptNext = true;
    ++m_state[playerId].discontinuity;
}

bool PlayerLocationService::isBypassed(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    return m_state[playerId].bypass;
}

std::uint32_t PlayerLocationService::getDiscontinuity(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    return m_state[playerId].discontinuity;
}

void PlayerLocationService::onSpawn(IPlayer &player)
{
    State &st = m_state[player.getID()];
    st.tracking = true;
    st.acceptNext = true; // спавн — легальный скачок в точку спавна
    st.pendingTeleport = false;
    st.position = player.getPosition();
    ++st.discontinuity;
}

void PlayerLocationService::onInteriorChange(IPlayer &player, unsigned newInterior, TimePoint now)
{
    State &st = m_state[player.getID()];
    if (st.interior == newInterior)
        return;
    st.interior = newInterior;

    // Enex-маркер легально телепортирует клиента — даём разовый грейс позиции,
    // но не чаще INTERIOR_GRACE_COOLDOWN: спам интерьерами не легализует телепорты.
    if (now - st.lastInteriorGrace >= INTERIOR_GRACE_COOLDOWN)
    {
        st.lastInteriorGrace = now;
        st.acceptNext = true;
    }
}

PlayerLocationService::VerifyOutcome PlayerLocationService::verify(IPlayer &player, TimePoint now)
{
    VerifyOutcome outcome;
    State &st = m_state[player.getID()];

    const PlayerState playerState = player.getState();
    if (!isPlayingState(playerState))
    {
        st.lastUpdate = now;
        return outcome;
    }

    const Vector3 reported = player.getPosition();

    // Байпас (редактор): позицию принимаем без проверок — источник правды
    // остаётся свежим, но каждый сэмпл помечен как разрыв непрерывности.
    if (st.bypass)
    {
        st.tracking = true;
        st.position = reported;
        st.lastUpdate = now;
        ++st.discontinuity;
        return outcome;
    }

    // Первый игровой апдейт или разовый грейс — принимаем как есть.
    if (!st.tracking || st.acceptNext)
    {
        st.tracking = true;
        st.acceptNext = false;
        st.position = reported;
        st.lastUpdate = now;
        ++st.discontinuity;
        return outcome;
    }

    const bool resumedFromPause =
        st.lastUpdate.time_since_epoch().count() != 0 && (now - st.lastUpdate) >= PAUSE_GAP;
    const float dt = clampDt(std::chrono::duration<float>(now - st.lastUpdate).count());
    st.lastUpdate = now;

    // Ожидание прибытия после серверного телепорта — РАНЬШЕ обработки паузы:
    // загрузка дальней зоны выглядит как пауза, но принимать позицию загрузки
    // как правду нельзя — правда на время грейса в точке телепорта.
    if (st.pendingTeleport)
    {
        if (resumedFromPause)
        {
            // Клиент грузил зону, а не игнорировал телепорт — продлеваем ожидание.
            st.teleportAt = now;
        }
        if (glm::distance(reported, st.teleportTarget) <= st.arriveRadius)
        {
            st.pendingTeleport = false;
            st.position = reported;
            ++st.discontinuity;
            return outcome;
        }
        if (now - st.teleportAt >= TELEPORT_GRACE)
        {
            // Клиент игнорирует setPosition — переустанавливаем и фиксируем.
            player.setPosition(st.teleportTarget);
            st.teleportAt = now;
            outcome.teleportHack = true;
            outcome.detail = fmt::format("ignoring teleport to {:.0f} {:.0f} {:.0f}", st.teleportTarget.x,
                                         st.teleportTarget.y, st.teleportTarget.z);
        }
        return outcome; // во время грейса правда — точка телепорта
    }

    // Пауза клиента: пешком на паузе игрок заморожен — позиция после возврата
    // должна совпадать с принятой (иначе «пауза + скачок» = бесплатный телепорт).
    // В транспорте принимаем как есть: пассажира на паузе машина легально увозит.
    if (resumedFromPause)
    {
        const bool inVehicle = playerState == PlayerState_Driver || playerState == PlayerState_Passenger;
        if (inVehicle || glm::distance(st.position, reported) <= PAUSE_MOVE_TOLERANCE)
        {
            st.position = reported;
            ++st.discontinuity;
            return outcome;
        }
        forceTo(player, st.position, now);
        outcome.teleportHack = true;
        outcome.detail = fmt::format("teleport during pause: {:.0f}m", glm::distance(st.position, reported));
        return outcome;
    }

    if (!inWorldBounds(reported))
    {
        forceTo(player, st.position, now);
        outcome.teleportHack = true;
        outcome.detail = fmt::format("out of world bounds: {:.0f} {:.0f} {:.0f}", reported.x, reported.y, reported.z);
        return outcome;
    }

    // Проверка достижимости: расстояние с прошлого апдейта против лимита скорости.
    const float dist = glm::distance(st.position, reported);
    const float maxSpeed =
        (playerState == PlayerState_Driver || playerState == PlayerState_Passenger) ? VEHICLE_MAX_SPEED
                                                                                    : FOOT_MAX_SPEED;
    const float allowed = maxSpeed * dt + DIST_SLACK;

    if (dist > allowed)
    {
        // Телепорт-хак: откатываем на последнюю принятую позицию (через механизм
        // телепорта — клиенту нужно время доехать до отката).
        forceTo(player, st.position, now);
        outcome.teleportHack = true;
        outcome.detail = fmt::format("moved {:.0f}m in {:.2f}s (max {:.0f}m)", dist, dt, allowed);
        return outcome;
    }

    st.position = reported; // правдоподобно — новая правда
    return outcome;
}

void PlayerLocationService::reset(int playerId)
{
    m_state[playerId] = State{};
}
