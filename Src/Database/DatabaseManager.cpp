#include "Database/DatabaseManager.h"

#include "Log/LogManager.h"
#include "ThreadPool/ThreadPool.h"
#include "core.hpp"
#include "mysqlx/devapi/settings.h"
#include "mysqlx/xdevapi.h"
#include <memory>

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

// Сессия возвращается в пул и запускается следующий отложенный запрос.
// Вызывается на главном потоке и при успехе, и при ошибке — поэтому пул
// не утекает, даже если запросы падают подряд.
void DatabaseManager::releaseAndPump(SessionWrapper *sessionWrapper)
{
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
        selectQuery(std::move(pending.task), std::move(pending.callback), std::move(pending.errorCallback));
    }
}

// Not thread safe, should be called from the main thread
void DatabaseManager::throwQuery(DatabaseManager::Task task, DatabaseManager::ErrorCallback errorCallback)
{
    SessionWrapper *sessionWrapper = m_sessionPool.get();
    if (sessionWrapper == nullptr)
    {
        m_queue.push({std::move(task), std::move(errorCallback)});
        return;
    }

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
}

// Not thread safe, should be called from the main thread
void DatabaseManager::selectQuery(DatabaseManager::SelectTask task, DatabaseManager::SelectCallback callback,
                                  DatabaseManager::ErrorCallback errorCallback)
{
    SessionWrapper *sessionWrapper = m_sessionPool.get();
    if (sessionWrapper == nullptr)
    {
        m_selectQueue.push({std::move(task), std::move(callback), std::move(errorCallback)});
        return;
    }

    ThreadPool::Task<mysqlx::RowResult> asyncTask;

    asyncTask.func = [task = std::move(task), sessionWrapper]()
    {
        return task(sessionWrapper->getSchema());
    };

    asyncTask.callback = [callback = std::move(callback), sessionWrapper](mysqlx::RowResult result)
    {
        callback(std::move(result));
        releaseAndPump(sessionWrapper);
    };

    asyncTask.errorCallback = [errorCallback = std::move(errorCallback), sessionWrapper](const std::string &error)
    {
        LogManager::log(Error, "DatabaseManager: select failed: " + error);
        if (errorCallback)
        {
            errorCallback(error);
        }
        releaseAndPump(sessionWrapper);
    };

    ThreadPool::addTask(std::move(asyncTask));
}
