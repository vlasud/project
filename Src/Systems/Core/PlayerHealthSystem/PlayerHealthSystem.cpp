#include "Systems/Core/PlayerHealthSystem/PlayerHealthSystem.h"

#include "Log/LogManager.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h" // единый источник темпа стрельбы
#include "Systems/Core/PlayerHealthSystem/WeaponLimits.h"
#include "glm/geometric.hpp"
#include <chrono>
#include <cmath>
#include <fmt/format.h>

namespace
{
// Допуск к дальности оружия: позиции на сервере отстают от клиентов (лаг,
// интерполяция, движение цели), поэтому даём щедрый запас — задача отсечь
// «выстрел через карту», а не честный хит на границе дальности.
constexpr float RANGE_SLACK = 25.0f;
constexpr float RANGE_FACTOR = 1.3f;

// Burst токен-бакета: столько хитов подряд можно «без ожидания». Покрывает
// сетевые батчи (клиент лагнул и его пакеты пришли пачкой).
constexpr float FIRE_BURST = 3.0f;

// Окно подтверждения попадания bullet sync'ом. Bullet sync и give-damage клиент
// шлёт в один момент, но это разные пакеты — даём запас на их рассинхрон.
constexpr std::chrono::milliseconds SHOT_WINDOW{2000};

// Урон от окружения (падение, огонь, утопление, взрыв) сообщает сам клиент, и в
// ядре этот путь не валидируется ВООБЩЕ: ни оружие, ни величина. Поэтому потолок и
// темп ставим здесь. Падение с максимальной высоты забирает около 100 HP, поэтому
// разовый урон выше этого — заявление, которое игра выдать не могла.
constexpr float MAX_ENVIRONMENT_DAMAGE = 110.0f;
// Токен-бакет темпа: окружение бьёт редкими событиями, а не потоком.
constexpr float ENVIRONMENT_BURST = 3.0f;
constexpr std::chrono::milliseconds ENVIRONMENT_INTERVAL{700};

// Осадка стейта для carshot: смену «в машине -> пешком» сервер узнаёт из следующего
// синка, поэтому хит сразу после высадки не считаем стрельбой из салона.
constexpr std::chrono::milliseconds STATE_SETTLE{1500};

TimePoint now()
{
    return std::chrono::steady_clock::now();
}
} // namespace

PlayerHealthSystem::PlayerHealthSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_healthService(serviceRegister.getService<PlayerHealthService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>())
{
    listen(core.getPlayers().getPlayerUpdateDispatcher(), this);
    listen(core.getPlayers().getPlayerSpawnDispatcher(), this);
    listen(core.getPlayers().getPlayerDamageDispatcher(), this);
    listen(core.getPlayers().getPlayerShotDispatcher(), this);
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);

    // Серверная смерть — единая точка для игровой логики (респаун, киллы,
    // статистика). Срабатывает и без клиентского onPlayerDeath.
    m_healthService.subscribeDeath(
        [](IPlayer &player)
        { LogManager::log(LogLevel::Message, fmt::format("[Health] Server death: {}", player.getName())); });
}

bool PlayerHealthSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    // Трек смены стейта для carshot-окна: держим здесь, а не в PlayerStateService —
    // нужна лишь метка времени по каждому синку, отдельного состояния это не стоит.
    AttackState &attack = m_attack[player.getID()];
    const PlayerState state = player.getState();
    if (state != attack.lastState)
    {
        attack.lastState = state;
        attack.stateSince = now;
    }

    PlayerHealthService::VerifyOutcome outcome = m_healthService.verify(player, now);
    if (outcome.healthHack)
    {
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::HealthHack,
                                  std::move(outcome.detail), now);
    }
    else if (outcome.deathEvasion)
    {
        m_antiCheatService.record(player.getID(), AntiCheatService::ViolationType::DeathEvasion,
                                  std::move(outcome.detail), now);
    }
    return true;
}

void PlayerHealthSystem::onPlayerSpawn(IPlayer &player)
{
    m_healthService.onSpawn(player.getID());
}

void PlayerHealthSystem::onPlayerDeath(IPlayer &player, IPlayer *killer, int reason)
{
    m_healthService.onClientDeath(player);
}

bool PlayerHealthSystem::onPlayerShotPlayer(IPlayer &player, IPlayer &target, const PlayerBulletData &bulletData)
{
    // Bullet sync атакующего: запоминаем цель — give-damage пулевым оружием
    // без подтверждённого выстрела по этой жертве будет отброшен.
    AttackState &attack = m_attack[player.getID()];
    attack.lastShotVictim = target.getID();
    attack.lastShotAt = now();
    return true;
}

bool PlayerHealthSystem::validateGiveDamage(IPlayer &attacker, IPlayer &victim, float amount, unsigned weapon)
{
    const TimePoint timeNow = now();
    const auto rejectAs = [&](AntiCheatService::ViolationType type, std::string detail)
    {
        m_antiCheatService.record(attacker.getID(), type, std::move(detail), timeNow);
        return false;
    };
    const auto reject = [&](std::string detail)
    { return rejectAs(AntiCheatService::ViolationType::DamageHack, std::move(detail)); };

    // NaN/Inf-урон open.mp пропускает (отсекает только <0). Табличный лимит ниже
    // его бы не поймал (NaN>max==false), а applyDamage отравил бы HP жертвы —
    // фиксируем фейк на атакующего и не применяем.
    if (!std::isfinite(amount))
    {
        return reject(fmt::format("non-finite damage {} from weapon {}", amount, weapon));
    }

    const WeaponLimits::Info *info = WeaponLimits::get(weapon);
    if (!info)
    {
        return reject(fmt::format("weapon {} cannot give damage", weapon));
    }

    if (amount > info->maxDamage + 1.0f)
    {
        return reject(fmt::format("weapon {} damage {:.1f} > max {:.1f}", weapon, amount, info->maxDamage));
    }

    // Темп хитов: токен-бакет. Пополняется со скоростью «1 хит в minInterval»,
    // запас burst покрывает легальные пакетные всплески; стабильный спам быстрее
    // скорострельности оружия упирается в пустой бакет.
    //
    // Для пулевого оружия интервал берём из ЕДИНОГО источника темпа
    // (PlayerWeaponService — реальный shootTime из SDK): своя, более щедрая копия
    // обесценивала бы строгий лимит на выстрелы — урон всё равно проходил бы чаще.
    const float intervalMs =
        WeaponLimits::sendsBulletSync(weapon)
            ? static_cast<float>(PlayerWeaponService::minShotInterval(static_cast<std::uint8_t>(weapon)).count())
            : static_cast<float>(info->minIntervalMs);

    AttackState &attack = m_attack[attacker.getID()];
    if (attack.lastRefill.time_since_epoch().count() != 0)
    {
        const float elapsedMs = std::chrono::duration<float, std::milli>(timeNow - attack.lastRefill).count();
        attack.fireTokens += elapsedMs / intervalMs;
        if (attack.fireTokens > FIRE_BURST)
            attack.fireTokens = FIRE_BURST;
    }
    attack.lastRefill = timeNow;
    if (attack.fireTokens < 1.0f)
    {
        return reject(fmt::format("weapon {} fire rate exceeded", weapon));
    }
    attack.fireTokens -= 1.0f;

    // Принятые позиции из источника правды: чит не сдвинет себя к жертве в обход.
    const float distance =
        glm::distance(m_locationService.getPosition(attacker.getID()), m_locationService.getPosition(victim.getID()));
    const float allowed = info->range * RANGE_FACTOR + RANGE_SLACK;
    if (distance > allowed)
    {
        return reject(fmt::format("weapon {} hit from {:.0f}m (max {:.0f}m)", weapon, distance, allowed));
    }

    // CarShot: из транспорта игра даёт стрелять только одноручным оружием. Хит
    // двуручным из салона честный клиент воспроизвести не может. Стейт берём
    // серверный, но с окном на осадку: высадку сервер видит на синк позже, а
    // выстрел сразу после неё легален.
    const PlayerState attackerState = attacker.getState();
    if ((attackerState == PlayerState_Driver || attackerState == PlayerState_Passenger)
        && WeaponLimits::sendsBulletSync(weapon) && !WeaponLimits::allowedInVehicle(weapon)
        && timeNow - attack.stateSince >= STATE_SETTLE)
    {
        return rejectAs(AntiCheatService::ViolationType::CarShot,
                        fmt::format("weapon {} hit from vehicle (drive-by impossible)", weapon));
    }

    if (WeaponLimits::sendsBulletSync(weapon))
    {
        const bool shotConfirmed =
            attack.lastShotVictim == victim.getID() && (timeNow - attack.lastShotAt) <= SHOT_WINDOW;
        if (!shotConfirmed)
        {
            return reject(fmt::format("weapon {} hit without bullet sync", weapon));
        }
    }

    return true;
}

void PlayerHealthSystem::onPlayerGiveDamage(IPlayer &player, IPlayer &to, float amount, unsigned weapon, BodyPart part)
{
    // Сообщает клиент АТАКУЮЩЕГО (player) об уроне по жертве (to). Жертва-god-mode
    // не может подавить это событие — поэтому именно отсюда применяем урон к ней
    // авторитетно. Но раз слово атакующего стало силой, сперва проверяем его на
    // правдоподобие — иначе фейковый give-damage стал бы кнопкой убийства.
    if (!validateGiveDamage(player, to, amount, weapon))
    {
        return;
    }

    m_healthService.applyDamage(to, amount);
}

void PlayerHealthSystem::onPlayerTakeDamage(IPlayer &player, IPlayer *from, float amount, unsigned weapon,
                                            BodyPart part)
{
    // P2P-урон уже применён через onPlayerGiveDamage от атакующего — здесь повторно
    // не применяем, иначе урон удвоится. Берём только урон без источника (падение,
    // огонь, утопление, столкновение) — для него give-события не существует.
    if (from != nullptr)
    {
        return;
    }

    // Этот канал целиком на слове клиента: ядро на ветке «без источника» не
    // проверяет ни оружие, ни величину урона (проверка веса стоит только когда
    // источник — другой игрок). Без валидации отсюда можно было убить себя одним
    // пакетом в любой момент — сорвать арест, отменить киллеру фраг, телепортнуться
    // на респавн — и, спамя мелкий урон, держать окно синхронизации HP вечно
    // открытым, глуша детектор HealthHack.
    const int playerId = player.getID();
    const TimePoint timeNow = now();
    const auto reject = [&](std::string detail)
    {
        m_antiCheatService.record(playerId, AntiCheatService::ViolationType::DamageHack, std::move(detail), timeNow);
    };

    if (!std::isfinite(amount) || amount <= 0.0f)
    {
        reject(fmt::format("non-finite environment damage {}", amount));
        return;
    }
    if (amount > MAX_ENVIRONMENT_DAMAGE)
    {
        reject(fmt::format("environment damage {:.1f} > max {:.1f} (weapon {})", amount, MAX_ENVIRONMENT_DAMAGE,
                           weapon));
        return;
    }

    AttackState &attack = m_attack[playerId];
    if (attack.environmentRefill.time_since_epoch().count() != 0)
    {
        const float elapsedMs = std::chrono::duration<float, std::milli>(timeNow - attack.environmentRefill).count();
        attack.environmentTokens +=
            elapsedMs / static_cast<float>(std::chrono::duration_cast<std::chrono::milliseconds>(ENVIRONMENT_INTERVAL).count());
        if (attack.environmentTokens > ENVIRONMENT_BURST)
            attack.environmentTokens = ENVIRONMENT_BURST;
    }
    attack.environmentRefill = timeNow;
    if (attack.environmentTokens < 1.0f)
    {
        reject(fmt::format("environment damage rate exceeded (weapon {})", weapon));
        return;
    }
    attack.environmentTokens -= 1.0f;

    m_healthService.applyDamage(player, amount);
}

void PlayerHealthSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_healthService.reset(player.getID());
    m_attack[player.getID()] = AttackState{};
}
