#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

struct ITask
{
    virtual ~ITask() = default;
    virtual void run() = 0;
    virtual void call() = 0;
};

class ThreadPool
{
  public:
    template <typename T> struct Task : ITask
    {
        std::function<T()> func;
        std::function<void(T)> callback;

      private:
        void run() override
        {
            result = func();
        }

        void call() override
        {
            if (result)
            {
                callback(std::move(*result));
            }
        }

        std::optional<T> result;
    };

    static void initialize(size_t threadCount);

    template <typename T> static void addTask(Task<T> task)
    {
        auto wrapped = std::make_unique<Task<T>>();
        wrapped->func = std::move(task.func);
        wrapped->callback = std::move(task.callback);

        std::lock_guard lock(m_mutex);
        m_tasks.push(std::move(wrapped));

        m_condition.notify_one();
    }

    static void shutdown();
    static void flush();

  private:
    static void workerThread();

    inline static std::vector<std::thread> m_threads;
    inline static std::atomic<bool> m_stop = false;
    inline static std::mutex m_mutex;
    inline static std::mutex m_callbackMutex;
    inline static std::condition_variable m_condition;
    inline static std::queue<std::unique_ptr<ITask>> m_tasks;
    inline static std::queue<std::unique_ptr<ITask>> m_completedTasks;
};
