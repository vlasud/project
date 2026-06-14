#include "Services/Core/PlayerHealthService/PlayerHealthService.h"

#include <chrono>
#include <cmath>
#include <fmt/format.h>

namespace
{
// Допуск на рассинхрон float и округление между клиентом и сервером.
constexpr float EPS = 1.0f;

// Окно на синхронизацию: после серверного изменения HP клиенту нужно несколько
// тиков, чтобы получить setHealth и отразить его в синхронизации. Пока окно не
// вышло, расхождение — это ещё не нарушение.
constexpr std::chrono::milliseconds SYNC_GRACE{1500};

// Окно ожидания подтверждения смерти (onPlayerDeath). Легальный клиент проигрывает
// анимацию смерти и шлёт RPC за пару секунд.
constexpr std::chrono::milliseconds DEATH_GRACE{5000};

// Разрыв между апдейтами, после которого считаем, что клиент был на паузе
// (Esc/alt-tab) и не обрабатывал наши setHealth. Без этого первый sync после паузы
// со старым HP дал бы ложный HealthHack на честного игрока.
constexpr std::chrono::milliseconds PAUSE_GAP{3000};

// Как часто фиксировать «играет после серверной смерти» (чтобы спам не раздувал
// журнал, но античит видел продолжающееся нарушение).
constexpr std::chrono::milliseconds DEAD_FLAG_INTERVAL{5000};

TimePoint now()
{
    return std::chrono::steady_clock::now();
}

bool isPlayingState(PlayerState state)
{
    // Валидируем HP только когда игрок реально в игре. Спектейт (во время
    // авторизации), смерть и переходы вход/выход из ТС пропускаем.
    return state == PlayerState_OnFoot || state == PlayerState_Driver || state == PlayerState_Passenger;
}
} // namespace

void PlayerHealthService::subscribeDeath(DeathHandler handler)
{
    m_deathHandlers.push_back(std::move(handler));
}

void PlayerHealthService::enterDying(IPlayer &player)
{
    State &st = m_state[player.getID()];
    if (st.dying)
        return;
    st.dying = true;

    // Серверная смерть объявляется здесь — ровно один раз, независимо от того,
    // пришлёт ли клиент onPlayerDeath.
    for (const DeathHandler &handler : m_deathHandlers)
        handler(player);
}

void PlayerHealthService::setHealth(IPlayer &player, float health)
{
    State &st = m_state[player.getID()];
    st.health = health < 0.0f ? 0.0f : health;
    st.lastChange = now();
    st.confirmed = false;
    player.setHealth(st.health);

    if (st.health <= 0.0f)
        enterDying(player); // серверное убийство
    else
        st.dying = false; // лечение отменяет незавершённую смерть
}

void PlayerHealthService::setArmour(IPlayer &player, float armour)
{
    State &st = m_state[player.getID()];
    st.armour = armour < 0.0f ? 0.0f : armour;
    st.lastChange = now();
    st.confirmed = false;
    player.setArmour(st.armour);
}

void PlayerHealthService::setInvulnerable(IPlayer &player, bool on)
{
    const int id = player.getID();
    if (id < 0 || id >= MAX_PLAYERS)
        return;
    m_state[id].invulnerable = on;
}

bool PlayerHealthService::isInvulnerable(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    return m_state[playerId].invulnerable;
}

void PlayerHealthService::applyDamage(IPlayer &player, float amount)
{
    if (amount <= 0.0f)
        return;

    State &st = m_state[player.getID()];
    if (!st.alive || st.dying)
        return;

    if (st.invulnerable)
    {
        // God mode: урон игнорируем, серверное HP неизменно. Клиент мог применить
        // урон локально — форсим серверное значение обратно (откат), чтобы HP не
        // разошёлся. Окно синхронизации — чтобы verify не счёл откат расхождением.
        st.lastChange = now();
        st.confirmed = false;
        player.setArmour(st.armour);
        player.setHealth(st.health);
        return;
    }

    // Модель GTA: урон сначала съедает броню, остаток уходит в HP.
    float remaining = amount;
    const float absorbed = st.armour < remaining ? st.armour : remaining;
    st.armour -= absorbed;
    remaining -= absorbed;
    st.health -= remaining;
    if (st.health < 0.0f)
        st.health = 0.0f;

    st.lastChange = now();
    st.confirmed = false;
    player.setArmour(st.armour);
    player.setHealth(st.health);

    if (st.health <= 0.0f)
        enterDying(player);
}

float PlayerHealthService::getHealth(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0.0f;
    return m_state[playerId].health;
}

float PlayerHealthService::getArmour(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0.0f;
    return m_state[playerId].armour;
}

bool PlayerHealthService::isAlive(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    // Серверно-авторитетно: как только серверное HP ушло в 0, игрок считается
    // мёртвым для всей логики — независимо от того, прислал ли клиент onPlayerDeath.
    const State &st = m_state[playerId];
    return st.alive && !st.dying;
}

void PlayerHealthService::onSpawn(int playerId)
{
    State &st = m_state[playerId];
    st.alive = true;
    st.dying = false;
    st.confirmed = false;
    st.health = 100.0f; // дефолт спавна GTA; кастомное HP — через setHealth()
    st.armour = 0.0f;
    st.lastChange = now();
    st.lastUpdate = st.lastChange;
    st.lastDeadFlag = st.lastChange;
}

void PlayerHealthService::onClientDeath(IPlayer &player)
{
    // Клиент подтвердил смерть. Если сервер её ещё не объявлял (мгновенная смерть
    // от окружения, которую сервер не видел), объявляем сейчас — событие уйдёт
    // подписчикам ровно один раз.
    State &st = m_state[player.getID()];
    if (st.alive)
        enterDying(player);

    st.alive = false;
    st.dying = false;
    st.confirmed = false;
    st.health = 0.0f;
    st.armour = 0.0f;
}

PlayerHealthService::VerifyOutcome PlayerHealthService::verify(IPlayer &player, TimePoint timeNow)
{
    VerifyOutcome outcome;

    State &st = m_state[player.getID()];

    if (!st.alive)
    {
        // Сервер уже закрыл смерть, а клиент продолжает слать игровые апдейты —
        // «ходячий труп». Честный мёртвый клиент onfoot-sync не шлёт.
        if (st.dying == false && isPlayingState(player.getState()) && timeNow - st.lastDeadFlag >= DEAD_FLAG_INTERVAL)
        {
            st.lastDeadFlag = timeNow;
            outcome.deathEvasion = true;
            outcome.detail = "playing while server-dead";
        }
        return outcome;
    }

    if (!isPlayingState(player.getState()))
        return outcome;

    // Пауза клиента: апдейты не шли — клиент не обрабатывал наши setHealth.
    // Возобновился — выдаём свежее окно синхронизации вместо мгновенного вердикта.
    const bool resumedFromPause =
        st.lastUpdate.time_since_epoch().count() != 0 && (timeNow - st.lastUpdate) >= PAUSE_GAP;
    st.lastUpdate = timeNow;
    if (resumedFromPause)
    {
        st.lastChange = timeNow;
        st.confirmed = false;
        return outcome;
    }

    // God mode: держим HP на серверном значении, любой локальный урон/рост клиента
    // откатываем, нарушения не пишем. Только для живого (dying пусть завершится).
    if (st.invulnerable && !st.dying)
    {
        const float rh = player.getHealth();
        const float ra = player.getArmour();
        if (std::abs((rh + ra) - (st.health + st.armour)) > EPS)
        {
            player.setHealth(st.health);
            player.setArmour(st.armour);
        }
        return outcome;
    }

    const float reportedHealth = player.getHealth();
    const float reportedArmour = player.getArmour();

    if (st.dying)
    {
        // Сервер свёл HP в 0 и ждёт onPlayerDeath. Клиент, который отказывается
        // умирать (игнорит setHealth(0) и не шлёт death-RPC), выявляется тут.
        if (reportedHealth > EPS)
        {
            // Заявляет, что жив, хотя сервер мёртв. Короткое окно на доставку
            // setHealth(0), затем — фиксация уклонения и повторный откат.
            if (timeNow - st.lastChange >= SYNC_GRACE)
            {
                st.lastChange = timeNow;
                player.setHealth(0.0f);
                player.setArmour(0.0f);
                outcome.deathEvasion = true;
                outcome.detail = fmt::format("ignoring death: server 0, reported {:.0f}+{:.0f}", reportedHealth,
                                             reportedArmour);
            }
            return outcome;
        }
        // Клиент на 0, но onPlayerDeath не пришёл. По истечении окна закрываем
        // смерть серверно и фиксируем неподтверждение — дальше любой игровой
        // апдейт от него будет ловиться веткой «ходячий труп» выше.
        if (timeNow - st.lastChange >= DEATH_GRACE)
        {
            st.alive = false;
            st.dying = false;
            st.lastDeadFlag = timeNow;
            outcome.deathEvasion = true;
            outcome.detail = "death not confirmed by client";
        }
        return outcome;
    }

    const float reportedTotal = reportedHealth + reportedArmour;
    const float serverTotal = st.health + st.armour;

    if (!st.confirmed)
    {
        if (std::abs(reportedTotal - serverTotal) <= EPS)
        {
            // Клиент сошёлся — принимаем его раскладку HP/брони (суммы совпали).
            st.confirmed = true;
            st.health = reportedHealth;
            st.armour = reportedArmour;
            return outcome;
        }
        if (timeNow - st.lastChange < SYNC_GRACE)
            return outcome;  // ещё сходится — ждём
        st.confirmed = true; // окно вышло — оцениваем как есть ниже
    }

    if (reportedTotal <= serverTotal + EPS)
    {
        // Снижение или равенство — легальный урон, который сервер не наблюдал
        // (падение, огонь, утопление, столкновение). Принимаем как новую правду.
        st.health = reportedHealth;
        st.armour = reportedArmour;
        if (st.health <= 0.0f)
            enterDying(player); // самоубился об окружение — смерть серверная сразу
        return outcome;
    }

    // Суммарный HP вырос без серверной санкции — god mode / health hack. Откатываем.
    st.lastChange = timeNow;
    st.confirmed = false;
    player.setHealth(st.health);
    player.setArmour(st.armour);

    outcome.healthHack = true;
    outcome.detail = fmt::format("server {:.0f}+{:.0f}, reported {:.0f}+{:.0f}", st.health, st.armour, reportedHealth,
                                 reportedArmour);
    return outcome;
}

void PlayerHealthService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_state[playerId] = State{};
}
