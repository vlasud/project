#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <functional>
#include <string>
#include <vector>

// Сервис здоровья — серверно-авторитетная модель HP/брони. В SA-MP здоровье
// клиент-авторитетно (клиент сообщает свой HP каждый sync), поэтому единственный
// способ убрать god mode — вести собственное значение на сервере, форсить его на
// клиент при уроне и сверять то, что клиент заявляет.
//
// Смерть тоже серверно-авторитетна: как только серверное HP достигает 0, игрок
// считается мёртвым (isAlive() == false) и срабатывает событие subscribeDeath —
// НЕ дожидаясь клиентского onPlayerDeath, который чит может не прислать вовсе.
// Игровая логика (респаун, киллы, статистика) должна слушать это событие, а не
// сырой клиентский RPC.
//
// КОНТРАКТ: любое легальное изменение HP/брони (хил, аптечка, броня, админ-команды,
// кастомный спавн) обязано идти ТОЛЬКО через этот сервис. Прямой player.setHealth()
// мимо сервиса валидатор воспримет как несанкционированный рост и зафиксирует чит.
//
// God mode (бессмертие) — серверный режим игнора урона: переключается ТОЛЬКО этим
// сервисом (setInvulnerable). Пока включён, applyDamage урон отбрасывает и форсит
// серверное HP назад, а verify держит HP на серверном значении без записи нарушения.
//
// Общий потолок HP — серверный максимум здоровья: setMaxHealth(cap) включает кэп,
// clearMaxHealth снимает. Пока кэп активен, серверное HP НИКОГДА не превышает
// maxHealth: зажим стоит во всех точках, где HP пишется вверх — setHealth, onSpawn
// (кэп переживает респавн, снимается только clearMaxHealth/reset) и verify (включая
// попытку клиента переписать раскладку «высокий HP + 0 брони» при той же сумме).
// Бронь под кэп НЕ попадает — её можно поднимать свободно. Кэп и god mode
// ортогональны. Семантика кэпа — чистая инфраструктура (без знания о причине).
class PlayerHealthService final : public IService
{
  public:
    // Срабатывает ровно один раз на смерть — в момент, когда СЕРВЕР счёл игрока
    // мёртвым (серверное HP достигло 0 или пришло клиентское подтверждение смерти,
    // смотря что раньше).
    using DeathHandler = std::function<void(IPlayer &player)>;
    void subscribeDeath(DeathHandler handler);

    // Авторизованные изменения. setHealth(0) — серверное убийство.
    void setHealth(IPlayer &player, float health);
    void setArmour(IPlayer &player, float armour);

    // God mode: переключатель серверного игнора урона (синхронизацию HP сделают
    // verify/applyDamage). isInvulnerable bounds-checked — вне диапазона false.
    void setInvulnerable(IPlayer &player, bool on);
    bool isInvulnerable(int playerId) const;

    // Общий потолок HP. setMaxHealth включает кэп и сразу зажимает текущее HP, если
    // оно выше cap (форсит клиент тем же путём, что setHealth). cap<0 трактуется как
    // 0. clearMaxHealth снимает кэп, текущее HP НЕ трогает (к клиенту не обращается).
    void setMaxHealth(IPlayer &player, float cap);
    void clearMaxHealth(int playerId);

    // Серверно-авторитетный урон: списывает броню, затем HP, и форсит клиент.
    // Вызывается игровой логикой и системой (по валидированному onPlayerGiveDamage
    // от атакующего — жертва-god-mode не может подавить чужой give-damage).
    void applyDamage(IPlayer &player, float amount);

    // Серверно-авторитетные значения (НЕ то, что заявляет клиент).
    float getHealth(int playerId) const;
    float getArmour(int playerId) const;
    bool isAlive(int playerId) const;

    // Результат сверки. healthHack — несанкционированный рост HP/брони.
    // deathEvasion — отказ умирать (игнор setHealth(0)) или игра после серверной
    // смерти. detail заполняется только при нарушении.
    struct VerifyOutcome
    {
        bool healthHack = false;
        bool deathEvasion = false;
        std::string detail;
    };

    // Вызываются PlayerHealthSystem.
    void onSpawn(int playerId);
    void onClientDeath(IPlayer &player); // клиент прислал onPlayerDeath
    VerifyOutcome verify(IPlayer &player, TimePoint now);
    void reset(int playerId);

  private:
    struct State
    {
        bool alive = false;        // слот в игре (заспавнен, не отключён)
        bool dying = false;        // серверное HP=0, ждём подтверждения смерти от клиента
        bool confirmed = false;    // клиент сошёлся к серверному значению
        bool invulnerable = false; // god mode (сбрасывается в reset через State{})
        bool capActive = false;    // активен общий потолок HP (см. maxHealth)
        float maxHealth = 0.0f; // потолок HP при capActive (бронь под него не попадает)
        float health = 0.0f;    // серверно-авторитетное HP
        float armour = 0.0f;    // серверно-авторитетная броня
        TimePoint lastChange;   // последнее серверное изменение (окно синхронизации)
        TimePoint lastUpdate;   // последний onPlayerUpdate (детект паузы клиента)
        TimePoint lastDeadFlag; // последняя фиксация «играет мёртвым» (rate limit)
    };

    // Переход в серверную смерть: выставляет dying и один раз зовёт подписчиков.
    void enterDying(IPlayer &player);

    std::array<State, MAX_PLAYERS> m_state;
    std::vector<DeathHandler> m_deathHandlers;
};
