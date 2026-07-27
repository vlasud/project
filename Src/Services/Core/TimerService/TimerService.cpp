#include "Services/Core/TimerService/TimerService.h"
#include "Log/LogManager.h"
#include "core.hpp"
#include <algorithm>

// Мост к компоненту: на каждый таймер создаётся свой обработчик, владеет им
// компонент — удаляет через free() при освобождении таймера.
class TimerService::HandlerImpl final : public TimerTimeOutHandler
{
  public:
    HandlerImpl(TimerService &service, int id) : m_service(&service), m_id(id)
    {
    }

    // Обработчиком владеет компонент, а он переживает геймод: обратный путь к
    // сервису обрывается его деструктором (detach), иначе освобождение таймера
    // после выгрузки позвало бы метод разрушенного объекта.
    void detach()
    {
        m_service = nullptr;
    }

    void timeout(ITimer &) override
    {
        if (m_service)
            m_service->handleTimeout(m_id);
    }

    void free(ITimer &) override
    {
        if (m_service)
            m_service->handleFree(m_id);
        delete this;
    }

  private:
    friend TimerService;        // только для delete при неудачном create
    ~HandlerImpl() = default;   // удаляется через free()

    TimerService *m_service;
    const int m_id;
};

// ------------------------------------------------------------------ public

TimerService::Handle TimerService::setTimeout(Milliseconds delay, Callback callback)
{
    return createTimer(delay, delay, 1, -1, std::move(callback), nullptr);
}

TimerService::Handle TimerService::setInterval(Milliseconds interval, Callback callback)
{
    return createTimer(interval, interval, 0, -1, std::move(callback), nullptr);
}

TimerService::Handle TimerService::setRepeating(Milliseconds initial, Milliseconds interval, unsigned int count,
                                                Callback callback)
{
    return createTimer(initial, interval, count, -1, std::move(callback), nullptr);
}

TimerService::Handle TimerService::setPlayerTimeout(IPlayer &player, Milliseconds delay, PlayerCallback callback)
{
    return createTimer(delay, delay, 1, player.getID(), nullptr, std::move(callback));
}

TimerService::Handle TimerService::setPlayerInterval(IPlayer &player, Milliseconds interval, PlayerCallback callback)
{
    return createTimer(interval, interval, 0, player.getID(), nullptr, std::move(callback));
}

void TimerService::cancel(Handle &handle)
{
    auto it = m_entries.find(handle.id);
    handle.id = 0;
    if (it == m_entries.end())
    {
        return;
    }

    // Только kill: запись удалит handleFree, когда компонент освободит таймер.
    if (it->second.timer->running())
    {
        it->second.timer->kill();
    }
}

bool TimerService::isActive(Handle handle) const
{
    auto it = m_entries.find(handle.id);
    return it != m_entries.end() && it->second.timer->running();
}

Milliseconds TimerService::remaining(Handle handle) const
{
    auto it = m_entries.find(handle.id);
    if (it == m_entries.end() || !it->second.timer->running())
    {
        return Milliseconds(0);
    }
    return it->second.timer->remaining();
}

// ------------------------------------------------------------------ private

TimerService::~TimerService()
{
    // Сначала флаг и отвязка обработчиков, потом kill: kill() ведёт к free() у
    // компонента, а тот шёл бы обратно в handleFree — то есть в разрушаемую мапу.
    // Компонент таймеров может освободить таймер и позже нас, поэтому отвязка
    // обработчиков обязательна: флаг читать было бы уже не у кого.
    m_shuttingDown = true;
    for (auto &[id, entry] : m_entries)
    {
        if (entry.handler)
        {
            entry.handler->detach();
        }
        if (entry.timer && entry.timer->running())
        {
            entry.timer->kill();
        }
    }
}

void TimerService::initialize(ICore *core, ITimersComponent *timers)
{
    m_core = core;
    m_timers = timers;
}

void TimerService::resetPlayer(int playerId)
{
    // kill без erase — итерация безопасна, записи удалит handleFree.
    for (auto &[id, entry] : m_entries)
    {
        if (entry.playerId == playerId && entry.timer->running())
        {
            entry.timer->kill();
        }
    }
}

TimerService::Handle TimerService::createTimer(Milliseconds initial, Milliseconds interval, unsigned int count,
                                               int playerId, Callback callback, PlayerCallback playerCallback)
{
    if (!m_timers)
    {
        LogManager::log(Error, "TimerService: ITimersComponent is missing, timer dropped");
        return Handle{};
    }

    initial = std::max(initial, Milliseconds(0));
    interval = std::max(interval, Milliseconds(0));

    const int id = ++m_nextId;
    HandlerImpl *handler = new HandlerImpl(*this, id);
    ITimer *timer = m_timers->create(handler, initial, interval, count);
    if (!timer)
    {
        delete handler;
        LogManager::log(Error, "TimerService: failed to create timer");
        return Handle{};
    }

    Entry entry;
    entry.timer = timer;
    entry.handler = handler;
    entry.playerId = playerId;
    entry.callback = std::move(callback);
    entry.playerCallback = std::move(playerCallback);
    m_entries[id] = std::move(entry);

    return Handle{id};
}

void TimerService::handleTimeout(int id)
{
    if (m_shuttingDown)
        return;
    auto it = m_entries.find(id);
    if (it == m_entries.end())
    {
        return;
    }
    // Ссылка стабильна: unordered_map не перемещает узлы при вставках из колбэка,
    // а erase происходит только в handleFree — после возврата из timeout.
    Entry &entry = it->second;

    if (entry.playerId >= 0)
    {
        IPlayer *player = m_core ? m_core->getPlayers().get(entry.playerId) : nullptr;
        if (!player)
        {
            // Гонка с дисконнектом в том же тике — гасим таймер.
            if (entry.timer->running())
            {
                entry.timer->kill();
            }
            return;
        }
        if (entry.playerCallback)
        {
            entry.playerCallback(*player); // последним действием: колбэк может отменять/создавать таймеры
        }
        return;
    }

    if (entry.callback)
    {
        entry.callback();
    }
}

void TimerService::handleFree(int id)
{
    if (m_shuttingDown)
        return; // мапа уже разрушается — стирать в ней нечего
    m_entries.erase(id);
}
