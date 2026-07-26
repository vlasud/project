#include "Systems/DeathPenaltySystem/DeathPenaltySystem.h"

#include <chrono>

DeathPenaltySystem::DeathPenaltySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_healthService(serviceRegister.getService<PlayerHealthService>()),
      m_timerService(serviceRegister.getService<TimerService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    listen(core.getPlayers().getPlayerSpawnDispatcher(), this);

    // Смерть серверно-авторитетна. Штраф здесь НЕ включаем (отсчёт от респавна) —
    // фиксируем ДОЛГ по аккаунту, чтобы выход до респавна не отменил штраф. Кэп и
    // таймер запускает onPlayerSpawn. Death-хэндлер срабатывает для живого
    // залогиненного игрока, поэтому аккаунт обычно валиден; гард по NO_ACCOUNT — на
    // всякий случай (смерть без сессии штрафовать некого).
    m_healthService.subscribeDeath(
        [this](IPlayer &player)
        {
            const AccountId account = m_sessionService.getAccountId(player.getID());
            if (account != PlayerSessionService::NO_ACCOUNT)
                m_penaltyOwed.insert(account);
        });
}

void DeathPenaltySystem::onPlayerSpawn(IPlayer &player)
{
    const int id = player.getID();
    const AccountId account = m_sessionService.getAccountId(id);
    if (account == PlayerSessionService::NO_ACCOUNT)
        return; // не залогинен — штраф привязан к аккаунту, на логин-спавне разберёмся

    const TimePoint now = std::chrono::steady_clock::now();

    // Есть долг (смерть в этой ИЛИ прошлой сессии — в т.ч. с выходом до респавна):
    // запускаем штраф от ЭТОГО респавна (каждая смерть перезапускает 5 минут,
    // перетирая прежнюю запись аккаунта). erase возвращает число снятых записей.
    if (m_penaltyOwed.erase(account) > 0)
        m_penaltyUntil[account] = now + PENALTY_DURATION;

    auto it = m_penaltyUntil.find(account);
    if (it == m_penaltyUntil.end() || it->second <= now)
    {
        // Активного штрафа нет: релог после истечения или спавн без смерти.
        // Протухшую запись подчищаем.
        if (it != m_penaltyUntil.end())
            m_penaltyUntil.erase(it);
        return;
    }

    // Активный штраф (свежий или продолженный после релога). Зажимаем HP к 10 —
    // core onSpawn уже отработал (HP=100) благодаря порядку регистрации, setMaxHealth
    // зажмёт и форсит клиент. Таймер на остаток снимет кэп.
    m_healthService.setMaxHealth(player, PENALTY_HEALTH);

    const TimePoint until = it->second;
    const Milliseconds remaining = std::chrono::duration_cast<Milliseconds>(until - now);
    m_timerService.setTimeout(remaining, [this, account, until]() { endPenalty(account, until); });
}

void DeathPenaltySystem::endPenalty(AccountId account, TimePoint expectedUntil)
{
    auto it = m_penaltyUntil.find(account);
    // Эпоха-гард: новая смерть перезаписала until (или штраф уже снят) — это
    // устаревший таймер, тихий no-op. Таймеры не отменяем — они безвредно отстреливают.
    if (it == m_penaltyUntil.end() || it->second != expectedUntil)
        return;
    if (std::chrono::steady_clock::now() < it->second)
        return; // защитно: время ещё не вышло

    // Игрок мог выйти — резолвим playerId через аккаунт (bounds-checked clearMaxHealth).
    // Offline → просто стираем запись (кэп его слота уже снял core reset на дисконнекте).
    const int pid = m_sessionService.playerByAccount(account);
    if (pid >= 0)
        m_healthService.clearMaxHealth(pid);
    m_penaltyUntil.erase(it);
}
