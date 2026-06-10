#include "ThreadPool.h"
#include "../Log/LogManager.h"
#include "fmt/base.h"

void ThreadPool::initialize(size_t threadCount)
{
    if (!m_threads.empty())
    {
        return;
    }

    // hardware_concurrency() может вернуть 0, и «hw - 1» у вызывающего кода
    // превращается в underflow size_t — страхуемся диапазоном.
    if (threadCount == 0 || threadCount > 64)
    {
        threadCount = 1;
    }

    m_threads.reserve(threadCount);

    for (size_t i = 0; i < threadCount; ++i)
    {
        m_threads.emplace_back(&ThreadPool::workerThread);
    }

    char text[70] = {0};
    fmt::format_to_n(text, sizeof(text), "Thread pool initialized with {} threads.", threadCount);
    LogManager::log(LogLevel::Message, text);
}

void ThreadPool::shutdown()
{
    {
        // m_stop ставится под тем же мьютексом, на котором спят воркеры — иначе
        // классический lost wakeup: воркер проверил предикат, ещё не уснул,
        // notify_all пролетел мимо, join виснет навсегда.
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_condition.notify_all();

    for (std::thread &thread : m_threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    m_threads.clear();

    // Воркеры дорабатывают очередь до конца (важно для записей в БД) —
    // выполняем колбэки доделанных задач на потоке, вызвавшем shutdown.
    flush();
}

// Вызывается на главном потоке (onTick): исполняет колбэки завершённых задач.
void ThreadPool::flush()
{
    // Горячий путь тика: на пустом пуле — одна атомарная загрузка, без мьютекса.
    if (m_pendingCallbacks.load(std::memory_order_acquire) == 0)
    {
        return;
    }

    std::queue<std::unique_ptr<ITask>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        tasks.swap(m_completedTasks);
    }

    int executed = 0;
    while (!tasks.empty())
    {
        ITask &task = *tasks.front();
        if (!task.error.empty())
        {
            bool handled = false;
            try
            {
                handled = task.fail();
            }
            catch (const std::exception &e)
            {
                handled = true;
                LogManager::log(LogLevel::Error, std::string("[ThreadPool] Error callback threw: ") + e.what());
            }
            catch (...)
            {
                handled = true;
                LogManager::log(LogLevel::Error, "[ThreadPool] Error callback threw unknown exception");
            }
            if (!handled)
            {
                LogManager::log(LogLevel::Error, "[ThreadPool] Unhandled task failure: " + task.error);
            }
        }
        else
        {
            // Исключение из колбэка не должно улетать в ядро сервера.
            try
            {
                task.call();
            }
            catch (const std::exception &e)
            {
                LogManager::log(LogLevel::Error, std::string("[ThreadPool] Callback threw: ") + e.what());
            }
            catch (...)
            {
                LogManager::log(LogLevel::Error, "[ThreadPool] Callback threw unknown exception");
            }
        }
        tasks.pop();
        ++executed;
    }

    m_pendingCallbacks.fetch_sub(executed, std::memory_order_relaxed);
}

void ThreadPool::workerThread()
{
    while (true)
    {
        std::unique_ptr<ITask> task;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock,
                             []
                             {
                                 return m_stop || !m_tasks.empty();
                             });

            // Очередь пуста — предикат сработал по m_stop: дренаж закончен, выходим.
            // Пока в очереди есть задачи, дорабатываем их даже после стопа.
            if (m_tasks.empty())
            {
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        // Исключение из задачи не должно ронять сервер (std::terminate).
        // Текст сохраняем в задачу — залогирует flush() на главном потоке.
        try
        {
            task->run();
        }
        catch (const std::exception &e)
        {
            task->error = e.what();
        }
        catch (...)
        {
            task->error = "unknown exception";
        }

        {
            std::lock_guard lock(m_callbackMutex);
            m_completedTasks.push(std::move(task));
        }
        m_pendingCallbacks.fetch_add(1, std::memory_order_release);
    }
}
