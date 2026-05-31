#include "ThreadPool.h"
#include "../Log/LogManager.h"
#include "fmt/base.h"
#include <functional>
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

void ThreadPool::addTask(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Task newTask{std::move(task), nullptr};
        m_tasks.emplace(std::move(newTask));
    }
    m_condition.notify_one();
}

void ThreadPool::addTaskWithResult(Task task)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.emplace(std::move(task));
    }
    m_condition.notify_one();
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
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    for (auto &callback : m_callbacks)
    {
        callback();
    }
}

void ThreadPool::workerThread()
{
    while (!m_stop)
    {
        Task task;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [] { return m_stop || !m_tasks.empty(); });

            if (m_stop && m_tasks.empty())
            {
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        task.asyncFunc();

        if (task.resultCallback)
        {
            std::lock_guard<std::mutex> callbackLock(m_callbackMutex);
            m_callbacks.push_back(std::move(task.resultCallback));
        }
    }
}
