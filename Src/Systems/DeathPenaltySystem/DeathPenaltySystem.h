#pragma once

#include "Macro.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <unordered_map>

// Штраф за смерть: после гибели НА РЕСПАВНЕ здоровье игрока зажимается в 10 HP и
// не может быть поднято ничем 5 минут (отсчёт от респавна). Кэпится ТОЛЬКО HP —
// бронь надевать можно. Реализуется поверх общего потолка HP PlayerHealthService
// (setMaxHealth/clearMaxHealth): эта система знает «смерть/штраф», сервис знает
// только «потолок».
//
// Смерть серверно-авторитетна — ловим её через PlayerHealthService::subscribeDeath
// (а не клиентский onPlayerDeath, который чит может не прислать). В death-хэндлере
// штраф НЕ ставим, только помечаем pending: отсчёт идёт от РЕСПАВНА, поэтому таймер
// и кэп включаются в onPlayerSpawn.
//
// Штраф переживает релог — ключ хранения это AccountId, а не playerId/слот. Запись
// «аккаунт -> когда штраф истекает» живёт в памяти и НЕ стирается на дисконнекте:
// перезашедший аккаунт с активным штрафом снова получит кэп на ближайшем спавне.
// Перезапуск сервера штраф сбрасывает (состояние только в памяти) — допустимо.
// Каждая новая смерть перезапускает 5 минут.
class DeathPenaltySystem : public BaseSystem, public PlayerSpawnEventHandler, public PlayerConnectEventHandler
{
  public:
    DeathPenaltySystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

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

    // Игрок умер, ждём его респавна, чтобы запустить штраф (индекс — getID()).
    std::array<bool, MAX_PLAYERS> m_pendingPenalty{};
    // Аккаунт -> момент окончания штрафа (steady_clock). Нет записи или прошлое =
    // штрафа нет. ПЕРЕЖИВАЕТ дисконнект (защита от релога).
    std::unordered_map<AccountId, TimePoint> m_penaltyUntil;
};
