#include "Systems/DeathPenaltySystem/DeathPenaltySystem.h"

#include <chrono>

DeathPenaltySystem::DeathPenaltySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_healthService(serviceRegister.getService<PlayerHealthService>()),
      m_timerService(serviceRegister.getService<TimerService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    // Смерть серверно-авторитетна. Штраф здесь НЕ ставим (отсчёт от респавна) —
    // лишь помечаем pending, чтобы onPlayerSpawn запустил кэп и таймер.
    m_healthService.subscribeDeath([this](IPlayer &player) { m_pendingPenalty[player.getID()] = true; });
}

void DeathPenaltySystem::onPlayerSpawn(IPlayer &player)
{
    const int id = player.getID();
    const AccountId account = m_sessionService.getAccountId(id);
    if (account == PlayerSessionService::NO_ACCOUNT)
        return; // не залогинен — штраф привязан к аккаунту, привязывать некуда

    const TimePoint now = std::chrono::steady_clock::now();

    // Свежая смерть: новый штраф отсчитывается от ЭТОГО респавна (каждая смерть
    // перезапускает 5 минут, перетирая прежнюю запись аккаунта).
    if (m_pendingPenalty[id])
    {
        m_pendingPenalty[id] = false;
        m_penaltyUntil[account] = now + PENALTY_DURATION;
    }

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

void DeathPenaltySystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    // Сбрасываем только pending слота (смерть-и-выход ДО респавна штраф не запускает).
    // m_penaltyUntil НЕ стираем — это защита от релога (ключ аккаунт, не слот). Кэп
    // здоровья этого слота снимет core PlayerHealthService::reset на дисконнекте.
    m_pendingPenalty[player.getID()] = false;
}
