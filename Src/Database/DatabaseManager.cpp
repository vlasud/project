#include "Database/DatabaseManager.h"

#include "Log/LogManager.h"
#include "ThreadPool/ThreadPool.h"
#include "core.hpp"
#include "mysqlx/devapi/settings.h"
#include "mysqlx/xdevapi.h"
#include <cassert>
#include <memory>
#include <thread>

namespace
{
const std::string HOST = "127.0.0.1";
const std::string USER = "root";
const std::string PASSWORD = "root";
const std::string DATABASE = "test";
const std::string PORT = "33060";
} // namespace

bool SessionWrapper::initialize()
{
    std::string uri = "mysqlx://" + USER + ":" + PASSWORD + "@" + HOST + ":" + PORT + "/" + DATABASE;
    try
    {
        m_session = std::make_unique<mysqlx::Session>(uri);
        return true;
    }
    catch (const std::exception &e)
    {
        LogManager::log(Error, std::string("Failed to open MySQL session: ") + e.what());
        return false;
    }
}

mysqlx::Schema SessionWrapper::getSchema()
{
    return m_session->getSchema(DATABASE);
}

void DatabaseManager::initialize()
{
    // initialize() вызывается из onLoad на главном потоке — фиксируем его id,
    // чтобы потом assertMainThread() ловил доступ к пулу/очередям с воркеров.
    s_mainThreadId = std::this_thread::get_id();

    size_t opened = 0;
    m_sessionPool.forEach(
        [&opened](SessionWrapper &wrapper)
        {
            if (wrapper.initialize())
            {
                ++opened;
            }
            return true;
        });

    if (opened == 0)
    {
        LogManager::log(Error, "DatabaseManager: no MySQL sessions could be opened. Check that MySQL X Protocol "
                               "(port 33060) is reachable and credentials are correct.");
        return;
    }

    LogManager::log(Message, "DatabaseManager initialized with " + std::to_string(opened) + " sessions");
}

// Проверка инварианта «доступ к пулу/очередям только с главного потока».
// До initialize() id ещё не зафиксирован — тогда не проверяем.
void DatabaseManager::assertMainThread()
{
    assert((s_mainThreadId == std::thread::id{} || std::this_thread::get_id() == s_mainThreadId) &&
           "DatabaseManager: pool/queue access must happen on the main thread");
}

// Сессия возвращается в пул и запускается следующий отложенный запрос.
// Вызывается на главном потоке и при успехе, и при ошибке — поэтому пул
// не утекает, даже если запросы падают подряд.
void DatabaseManager::releaseAndPump(SessionWrapper *sessionWrapper)
{
    assertMainThread();
    m_sessionPool.release(sessionWrapper);

    if (!m_queue.empty())
    {
        PendingThrow pending = std::move(m_queue.front());
        m_queue.pop();
        throwQuery(std::move(pending.task), std::move(pending.errorCallback));
    }
    else if (!m_selectQueue.empty())
    {
        PendingSelect pending = std::move(m_selectQueue.front());
        m_selectQueue.pop();
        pending.retry(); // повторно вызовет selectQuery<T> — сессия уже свободна
    }
}

// Not thread safe, should be called from the main thread
void DatabaseManager::throwQuery(DatabaseManager::Task task, DatabaseManager::ErrorCallback errorCallback)
{
    assertMainThread();
    SessionWrapper *sessionWrapper = m_sessionPool.get();
    if (sessionWrapper == nullptr)
    {
        m_queue.push({std::move(task), std::move(errorCallback)});
        return;
    }

    // Гард вернёт сессию в пул, если addTask бросит до постановки задачи.
    SessionGuard guard(sessionWrapper);

    ThreadPool::Task<bool> asyncTask;

    asyncTask.func = [task = std::move(task), sessionWrapper]()
    {
        task(sessionWrapper->getSchema());
        return true;
    };

    asyncTask.callback = [sessionWrapper](bool)
    {
        releaseAndPump(sessionWrapper);
    };

    asyncTask.errorCallback = [errorCallback = std::move(errorCallback), sessionWrapper](const std::string &error)
    {
        LogManager::log(Error, "DatabaseManager: query failed: " + error);
        if (errorCallback)
        {
            errorCallback(error);
        }
        releaseAndPump(sessionWrapper);
    };

    ThreadPool::addTask(std::move(asyncTask));
    guard.commit(); // задача принята — релиз теперь делает releaseAndPump в её колбэке
}

// selectQuery<T> / dispatchSelect<T> — шаблонные, определены в DatabaseManager.h.
