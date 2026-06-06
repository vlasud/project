#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool
{
  public:
    struct Task
    {
        std::function<void()> func;
        std::function<void()> callback;
    };

    static void initialize(size_t threadCount);
    static void addTask(std::function<void()> task);
    static void addTaskWithCallback(Task task);
    static void shutdown();
    static void flush();

  private:
    static void workerThread();

    inline static std::vector<std::thread> m_threads;
    inline static std::atomic<bool> m_stop = false;
    inline static std::mutex m_mutex;
    inline static std::mutex m_callbackMutex;
    inline static std::condition_variable m_condition;
    inline static std::queue<Task> m_tasks;
    inline static std::vector<std::function<void()>> m_callbacks;
};
