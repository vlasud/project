#include "ThreadPool.h"
#include "../Log/LogManager.h"
#include "fmt/base.h"
#include <mutex>

void ThreadPool::initialize(const size_t threadCount)
{
    if (!m_threads.empty())
    {
        return;
    }

    m_threads.reserve(threadCount);

    for (size_t i = 0; i < threadCount; ++i)
    {
        std::thread thread(&ThreadPool::workerThread);
        m_threads.emplace_back(std::move(thread));
    }

    char text[70] = {0};
    fmt::format_to_n(text, sizeof(text), "Thread pool initialized with {} threads.", threadCount);
    LogManager::log(LogLevel::Message, text);
}

void ThreadPool::shutdown()
{
    m_stop = true;
    m_condition.notify_all();

    for (std::thread &thread : m_threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    m_threads.clear();
}

// This method should be called from the main thread to execute result callbacks after worker threads have completed
// their tasks.
void ThreadPool::flush()
{
    std::queue<std::unique_ptr<ITask>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        tasks.swap(m_completedTasks);
    }

    while (!tasks.empty())
    {
        tasks.front()->call();
        tasks.pop();
    }
}

void ThreadPool::workerThread()
{
    while (!m_stop)
    {
        std::unique_ptr<ITask> task;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock,
                             []
                             {
                                 return m_stop || !m_tasks.empty();
                             });

            if (m_stop && m_tasks.empty())
            {
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        task->run();

        {
            std::lock_guard lock(m_callbackMutex);
            m_completedTasks.push(std::move(task));
        }
    }
}
