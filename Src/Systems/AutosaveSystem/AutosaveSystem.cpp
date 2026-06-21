#include "Systems/AutosaveSystem/AutosaveSystem.h"

AutosaveSystem::AutosaveSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_timerService(serviceRegister.getService<TimerService>())
{
}

void AutosaveSystem::initialize(IComponentList *components)
{
    // Таймер ставим в initialize (не в конструкторе): к этому моменту TimerSystem
    // уже передал сервису компонент таймеров. Колбэк на главном потоке — отмена
    // не нужна (таймер живёт всю работу сервера).
    m_timer = m_timerService.setInterval(AUTOSAVE_INTERVAL, [this] { saveAll(); });
}

void AutosaveSystem::saveAll()
{
    // Итерируем валидных игроков из пула — save() сам гардится isActive (бот/не
    // залогиненный — ранний return), а каждый персистер гардится своим m_loaded
    // (автосейв только-что-зашедшего, чья загрузка не дошла, не затрёт БД).
    for (IPlayer *player : m_core.getPlayers().entries())
    {
        if (m_sessionService.isActive(player->getID()))
            m_sessionService.save(*player);
    }
}
