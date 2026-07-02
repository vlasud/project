#include "Services/Core/PlayerLocationService/PlayerLocationService.h"

#include "glm/geometric.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
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

// Грейс-ОКНО позиции на клиентский телепорт машины с водителем в интерьер мод-шопа
// и обратно (SCM). Не однократный acceptNext: SCM-событие и телепорт-синк идут
// разными пакетами, и одноразовый грейс мог бы «съесть» стрей-синк с воротной
// позицией до фактического телепорта, зафлажив честного тюнера. Окно покрывает всю
// фазу enter→fade→интерьер (симметрично exit).
constexpr std::chrono::milliseconds MODSHOP_TELEPORT_GRACE{5000};

// Радиус прибытия: не больше этого, и не больше 40% дальности прыжка — иначе при
// коротком откате (10-20 м) запоздавшие пакеты с читерской позицией сами попадают
// в радиус и засчитываются как «прибыл», порождая каскад ложных нарушений.
constexpr float ARRIVE_MAX = 30.0f;
constexpr float ARRIVE_MIN = 5.0f;
constexpr float ARRIVE_FRACTION = 0.4f;

// Пешком после паузы позиция не должна была измениться (на паузе клиент заморожен).
// Допуск щедрый — на рассинхрон и редкие толчки. Только для пешего: в транспорте
// машину за время разрыва (Esc-пауза или сетевой лаг) могло легально унести, поэтому
// там допуск считается по достижимости VEHICLE_MAX_SPEED × время разрыва, а не отсюда.
constexpr float PAUSE_MOVE_TOLERANCE = 100.0f;

// Потолок «времени разрыва» для расчёта допустимого смещения в транспорте на
// паузе: лаг редко длиннее, а долгая Esc-пауза машину не двигает (dist≈0 и так
// проходит). Ограничивает допуск, чтобы длинная фейк-пауза не давала огромный.
constexpr float MAX_PAUSE_REACH_SECONDS = 12.0f;

float clampDt(float seconds)
{
    if (seconds < 0.01f)
        return 0.01f;
    if (seconds > 1.0f)
        return 1.0f;
    return seconds;
}

bool isFinite(const Vector3 &p)
{
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
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

    // Не-конечная цель (NaN/Inf) НИКОГДА не пишется в источник правды: ниже по
    // потоку cellCoord делает static_cast<int>(position) — на NaN/Inf это UB.
    // Откатываем на последнюю принятую позицию, телепорт не инициируем.
    if (!isFinite(position))
    {
        player.setPosition(st.position);
        return;
    }

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

void PlayerLocationService::grantModShopTeleportGrace(int playerId, TimePoint now)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    State &st = m_state[playerId];
    // Временное ОКНО, не однократный acceptNext и не pendingTeleport: точку
    // прибытия (интерьер шопа/ворота) диктует клиент, а не сервер — ждать
    // конкретный teleportTarget нечем; а SCM-событие приходит отдельным пакетом
    // от driver-sync, поэтому одноразовый грейс мог бы уйти на стрей-синк с
    // прежней позицией до фактического телепорта. Пока окно открыто, verify
    // принимает любой конечный синк как правду (разрыв непрерывности — там же).
    st.modShopGraceUntil = now + MODSHOP_TELEPORT_GRACE;
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
    // Не-конечную позицию в источник правды не пишем (инвариант cellCoord); грейс
    // acceptNext всё равно примет первый конечный апдейт.
    const Vector3 spawnPos = player.getPosition();
    if (isFinite(spawnPos))
        st.position = spawnPos;
    ++st.discontinuity;
}

void PlayerLocationService::onInteriorChange(IPlayer &player, unsigned newInterior, TimePoint now)
{
    // Чисто клиентский RPC. Встроенные enex-маркеры GTA выключены (WorldSystem) —
    // легальной клиентской смены интерьера с переносом позиции не бывает; серверные
    // переносы (двери баз, телепорты) идут через teleport()/setInterior() и берут
    // грейс позиции из pendingTeleport. Поэтому здесь грейс позиции НЕ выдаём: иначе
    // чит чередованием интерьеров (1/0/1/0) легализовал бы телепорт куда угодно.
    // Обновляем только источник правды об интерьере; позицию валидирует обычная проверка.
    (void)now;
    m_state[player.getID()].interior = newInterior;
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

    // Не-конечная клиентская позиция (NaN/Inf) НИКОГДА не доходит до st.position:
    // ниже cellCoord делает static_cast<int> из координат — на NaN/Inf это UB.
    // Отклоняем на ВСЕХ путях (байпас/грейс/обычная проверка) ДО любой записи,
    // оставляя последнюю принятую позицию. В байпасе (редактор) только не пишем —
    // не дёргаем setPosition и не плодим нарушение (дев двигает игрока сам); вне
    // байпаса форсим клиента обратно в принятую точку и фиксируем нарушение.
    if (!isFinite(reported))
    {
        st.lastUpdate = now;
        if (!st.bypass)
        {
            forceTo(player, st.position, now);
            outcome.teleportHack = true;
            outcome.detail = "non-finite position reported";
        }
        return outcome;
    }

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

    // Первый игровой апдейт или разовый грейс (спавн/байпас — серверные скачки;
    // мод-шоп — легальный клиентский, см. grantModShopTeleportGrace) —
    // принимаем как есть.
    if (!st.tracking || st.acceptNext)
    {
        st.tracking = true;
        st.acceptNext = false;
        st.position = reported;
        st.lastUpdate = now;
        ++st.discontinuity;
        return outcome;
    }

    // Грейс-окно мод-шопа (клиентский телепорт машины с водителем в интерьер и
    // обратно): пока открыто — принимаем любой конечный синк как правду. ОКНО, а
    // не однократный acceptNext, чтобы стрей-синк между принятым SCM-enter и
    // телепортом не потратил грейс и не дал ложный teleportHack (см.
    // grantModShopTeleportGrace). Заработано dwell'ом машины в реальной зоне шопа.
    if (st.modShopGraceUntil.time_since_epoch().count() != 0 && now < st.modShopGraceUntil)
    {
        st.tracking = true;
        st.position = reported;
        st.lastUpdate = now;
        ++st.discontinuity;
        return outcome;
    }

    const bool resumedFromPause =
        st.lastUpdate.time_since_epoch().count() != 0 && (now - st.lastUpdate) >= PAUSE_GAP;
    // Реальное время разрыва — для расчёта достижимого смещения в транспорте на паузе.
    // Отдельно от dt: dt клампится к 1с (анти-спидхак), а нам нужен фактический разрыв.
    const float pauseSeconds = std::chrono::duration<float>(now - st.lastUpdate).count();
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

    // Пауза клиента (Esc) ИЛИ сетевой лаг: разрыв sync >= PAUSE_GAP, dt не копится.
    // В транспорте за это время машину могло легально унести, пеший — заморожен.
    if (resumedFromPause)
    {
        // В транспорте машина за время разрыва могла легально уехать: при сетевом
        // лаге клиент-водитель продолжает движение, пассажира везёт водитель.
        // Допуск — по машинной скорости за ФАКТИЧЕСКОЕ время разрыва (с потолком).
        // Так лагающего честного водителя не выбьет из ТС, легитимный пассажир
        // принимается, а чит-телепорт через карту превышает достижимое и ловится —
        // одинаково для водителя и пассажира (закрывает и обход через машину без
        // водителя). Пеший на паузе заморожен — для него жёсткий допуск ниже.
        if (playerState == PlayerState_Driver || playerState == PlayerState_Passenger)
        {
            const float reach = VEHICLE_MAX_SPEED * std::min(pauseSeconds, MAX_PAUSE_REACH_SECONDS) + DIST_SLACK;
            if (inWorldBounds(reported) && glm::distance(st.position, reported) <= reach)
            {
                st.position = reported;
                ++st.discontinuity;
                return outcome;
            }
            forceTo(player, st.position, now);
            outcome.teleportHack = true;
            outcome.detail = fmt::format("teleport during pause in vehicle: {:.0f}m (max {:.0f}m over {:.1f}s)",
                                         glm::distance(st.position, reported), reach,
                                         std::min(pauseSeconds, MAX_PAUSE_REACH_SECONDS));
            return outcome;
        }

        // Пеший на паузе заморожен — позиция после возврата должна совпасть с принятой.
        if (glm::distance(st.position, reported) <= PAUSE_MOVE_TOLERANCE)
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
