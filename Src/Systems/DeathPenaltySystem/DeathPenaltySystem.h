#pragma once

#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include "types.hpp"
#include <unordered_map>
#include <unordered_set>

// Штраф за смерть: после гибели НА РЕСПАВНЕ здоровье игрока зажимается в 10 HP и
// не может быть поднято ничем 5 минут (отсчёт от респавна). Кэпится ТОЛЬКО HP —
// бронь надевать можно. Реализуется поверх общего потолка HP PlayerHealthService
// (setMaxHealth/clearMaxHealth): эта система знает «смерть/штраф», сервис знает
// только «потолок».
//
// Смерть серверно-авторитетна — ловим её через PlayerHealthService::subscribeDeath
// (а не клиентский onPlayerDeath, который чит может не прислать). В death-хэндлере
// штраф НЕ включаем (отсчёт идёт от РЕСПАВНА), но фиксируем ДОЛГ по аккаунту
// (m_penaltyOwed); кэп и таймер включаются в onPlayerSpawn. Долг по АККАУНТУ, а не
// по слоту, чтобы выход на экране «wasted» ДО респавна не отменял штраф: при
// перезаходе он применится на ближайшем спавне — увильнуть выходом нельзя.
//
// Штраф переживает релог — и долг (m_penaltyOwed), и активный остаток
// (m_penaltyUntil) хранятся по AccountId и НЕ стираются на дисконнекте: перезашедший
// аккаунт получит/продолжит штраф на ближайшем спавне. Перезапуск сервера штраф
// сбрасывает (состояние только в памяти) — допустимо. Каждая смерть перезапускает 5
// минут (от соответствующего респавна).
class DeathPenaltySystem : public BaseSystem, public PlayerSpawnEventHandler
{
  public:
    DeathPenaltySystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerSpawn(IPlayer &player) override;

  private:
    using AccountId = PlayerSessionService::AccountId;

    // Здоровье под штрафом и длительность кэпа от респавна (баланс-константы).
    static constexpr float PENALTY_HEALTH = 10.0f;
    static constexpr Minutes PENALTY_DURATION{5};

    // Снять штраф у аккаунта. expectedUntil — эпоха-гард: если запись штрафа уже
    // другая (вытеснена новой смертью) или удалена, колбэк — безвредный no-op.
    void endPenalty(AccountId account, TimePoint expectedUntil);

    PlayerHealthService &m_healthService;
    TimerService &m_timerService;
    PlayerSessionService &m_sessionService;

    // Аккаунты, которые умерли и ДОЛЖНЫ штраф, но ещё не получили кэп (ждут
    // ближайшего спавна). По аккаунту, а не слоту — переживает выход до респавна.
    std::unordered_set<AccountId> m_penaltyOwed;
    // Аккаунт -> момент окончания АКТИВНОГО штрафа (steady_clock). Нет записи или
    // прошлое = активного штрафа нет. ПЕРЕЖИВАЕТ дисконнект (защита от релога).
    std::unordered_map<AccountId, TimePoint> m_penaltyUntil;
};
