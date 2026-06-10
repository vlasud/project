#pragma once

#include "../../Services/AntiCheatService/AntiCheatService.h"
#include "../../Services/PlayerHealthService/PlayerHealthService.h"
#include "../BaseSystem.h"
#include "player.hpp"
#include <array>

// Связывает сервис здоровья с событиями игрока: спавн/смерть переключают модель,
// на каждом апдейте сверяет заявленный HP с серверным, нарушения пишет в журнал.
//
// Урон применяется по onPlayerGiveDamage (его шлёт клиент АТАКУЮЩЕГО), а не по
// onPlayerTakeDamage (его шлёт клиент жертвы). Жертва-god-mode может не слать
// take-damage вовсе, но не может подавить give-damage честного стрелка.
//
// Раз give-damage авторитетен, ЕГО НУЖНО ВАЛИДИРОВАТЬ — иначе чит-атакующий
// получает кнопку удалённого убийства. Перед применением хит проверяется на
// правдоподобие: урон не выше табличного для оружия, темп не выше скорострельности
// (токен-бакет, устойчив к сетевым батчам), дистанция в пределах дальности оружия,
// а для пулевого оружия попадание должно подтверждаться bullet sync
// (onPlayerShotPlayer) по той же жертве. Фейковый хит отбрасывается и пишется
// в журнал как DamageHack.
//
// onPlayerTakeDamage используем только для урона без источника (падение, огонь,
// утопление) — для него give-события не существует.
class PlayerHealthSystem : public BaseSystem,
                           public PlayerUpdateEventHandler,
                           public PlayerSpawnEventHandler,
                           public PlayerDamageEventHandler,
                           public PlayerShotEventHandler,
                           public PlayerConnectEventHandler
{
  public:
    PlayerHealthSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDeath(IPlayer &player, IPlayer *killer, int reason) override;
    void onPlayerGiveDamage(IPlayer &player, IPlayer &to, float amount, unsigned weapon, BodyPart part) override;
    void onPlayerTakeDamage(IPlayer &player, IPlayer *from, float amount, unsigned weapon, BodyPart part) override;
    bool onPlayerShotPlayer(IPlayer &player, IPlayer &target, const PlayerBulletData &bulletData) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    // Анти-fake-damage состояние атакующего.
    struct AttackState
    {
        float fireTokens = 3.0f; // токен-бакет темпа хитов (burst до 3)
        TimePoint lastRefill;
        int lastShotVictim = -1; // последняя цель из bullet sync
        TimePoint lastShotAt;
    };

    // Проверка правдоподобия хита; при фейке пишет DamageHack и возвращает false.
    bool validateGiveDamage(IPlayer &attacker, IPlayer &victim, float amount, unsigned weapon);

    PlayerHealthService &m_healthService;
    AntiCheatService &m_antiCheatService;

    std::array<AttackState, MAX_PLAYERS> m_attack;
};
