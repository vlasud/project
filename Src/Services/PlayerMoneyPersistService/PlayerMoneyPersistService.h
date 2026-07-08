#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include <array>
#include <functional>
#include <vector>

class PlayerMoneyPersistSystem;

// Кэш загруженных из БД наличных аккаунта. Бизнес-фича персиста, НЕ Core:
// PlayerMoneyService остаётся чистым рантайм-состоянием без доступа к БД (см.
// Docs/Persistence.md); саму загрузку/сохранение делает PlayerMoneyPersistSystem.
// Наличные не сбрасываются на спавне — PlayerMoneyPersistSystem пишет их в
// PlayerMoneyService СРАЗУ на загрузке (isMoneyLoaded уже означает «в сервисе
// реальный баланс»).
//
// Тайминг применения и гонка с загрузкой: сессия стартует и загрузка (async-
// select) уходит на воркер, а логин-спавн игрока — отдельный клиентский
// round-trip (finalize -> setSpectating(false) -> ожидание onPlayerSpawn). В
// подавляющем большинстве случаев БД успевает раньше. На случай обратного —
// спавн пришёл РАНЬШЕ загрузки — PlayerAuthSystem подписывается на
// subscribeMoneyLoaded: наблюдатель применит кэш, как только загрузка
// догонит (сам PlayerAuthSystem решает, ждёт он этого события или нет, —
// сервис лишь оповещает КАЖДУЮ загрузку, включая штатный случай).
class PlayerMoneyPersistService final : public IService
{
    friend PlayerMoneyPersistSystem;

  public:
    // --- чтение кэша (для применения в PlayerAuthSystem) ---
    bool isMoneyLoaded(int playerId) const;
    unsigned long long cachedMoney(int playerId) const;

    // --- наблюдатель: загрузка завершилась (в т.ч. late — после уже
    // пройденного спавна). Зовётся ПОСЛЕ serial-guard в PlayerMoneyPersistSystem
    // — playerId гарантированно принадлежит текущей сессии слота.
    using MoneyObserver = std::function<void(IPlayer &, unsigned long long cash)>;
    void subscribeMoneyLoaded(MoneyObserver observer);

  private:
    // --- вызывается PlayerMoneyPersistSystem ---
    void loadMoney(IPlayer &player, unsigned long long cash);
    void reset(int playerId);

    struct MoneyCache
    {
        bool loaded = false;
        unsigned long long cash = 0;
    };

    std::array<MoneyCache, MAX_PLAYERS> m_money;
    std::vector<MoneyObserver> m_moneyObservers;
};
