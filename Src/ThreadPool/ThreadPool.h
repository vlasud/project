#pragma once

#include "Log/LogManager.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

struct ITask
{
    virtual ~ITask() = default;
    virtual void run() = 0;
    virtual void call() = 0;

    // Обработка ошибки на главном потоке. Возвращает false, если обработчика
    // нет — тогда flush() просто залогирует.
    virtual bool fail() = 0;

    // Текст исключения из run(); заполняется воркером, логируется/передаётся в
    // fail() в flush() на главном потоке (логгер ядра не обязан быть
    // потокобезопасным).
    std::string error;
};

class ThreadPool
{
  public:
    // func выполняется на воркере; на главном потоке (из flush()) гарантированно
    // вызывается РОВНО ОДИН из двух: callback(result) при успехе или
    // errorCallback(текст исключения) при ошибке. Если errorCallback не задан,
    // ошибка просто логируется.
    template <typename T> struct Task : ITask
    {
        std::function<T()> func;
        std::function<void(T)> callback;
        std::function<void(const std::string &)> errorCallback;

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

        bool fail() override
        {
            if (!errorCallback)
            {
                return false;
            }
            errorCallback(error);
            return true;
        }

        std::optional<T> result;
    };

    static void initialize(size_t threadCount);

    template <typename T> static void addTask(Task<T> task)
    {
        auto wrapped = std::make_unique<Task<T>>(std::move(task));
        {
            std::lock_guard lock(m_mutex);
            // Воркеров уже нет. Если идёт дренаж shutdown() (m_draining) — задачу
            // заберёт он же на следующей итерации, это штатно, молчим. Иначе это
            // вызов уже ПОСЛЕ возврата из shutdown(): задача осядет навсегда —
            // логируем, чтобы потеря была видна, а не тиха.
            if (m_stop && m_threads.empty() && !m_draining)
            {
                LogManager::log(LogLevel::Warning,
                                "[ThreadPool] addTask after shutdown with no workers; task will stay "
                                "queued forever and never run");
            }
            m_tasks.push(std::move(wrapped));
        }
        // notify вне мьютекса: разбуженный воркер сразу возьмёт лок,
        // а не упрётся в занятый нами.
        m_condition.notify_one();
    }

    static void shutdown();
    static void flush();

  private:
    static void workerThread();

    // Дренаж shutdown(): синхронно прогоняет всё, что накопилось в m_tasks,
    // на вызывающем потоке — воркеров уже нет, ждать некому.
    static void runQueuedTasksSync();

    inline static std::vector<std::thread> m_threads;
    inline static std::atomic<bool> m_stop = false;
    inline static std::mutex m_mutex;
    inline static std::mutex m_callbackMutex;
    inline static std::condition_variable m_condition;
    inline static std::queue<std::unique_ptr<ITask>> m_tasks;
    inline static std::queue<std::unique_ptr<ITask>> m_completedTasks;

    // Сколько задач ждёт колбэка: flush() на каждом тике выходит по нулю
    // без захвата мьютекса.
    inline static std::atomic<int> m_pendingCallbacks = 0;

    // true, пока крутится дренаж-цикл shutdown(): реентрантный addTask из колбэка
    // будет подобран этим же дренажом, поэтому warning о «потерянной» задаче в это
    // время не пишем — иначе штатная остановка с backpressure спамит логи.
    inline static std::atomic<bool> m_draining = false;
};
