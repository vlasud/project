#include "DatabaseManager.h"

#include "../Log/LogManager.h"
#include "../ThreadPool/ThreadPool.h"
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

// Not thread safe, should be called from the main thread
void DatabaseManager::throwQuery(DatabaseManager::Task task)
{
    SessionWrapper *sessionWrapper = m_sessionPool.get();
    if (sessionWrapper == nullptr)
    {
        m_queue.push(task);
        return;
    }

    ThreadPool::Task<bool> asyncTask;

    asyncTask.func = [task = std::move(task), sessionWrapper]()
    {
        task(sessionWrapper->getSchema());
        return true;
    };

    asyncTask.callback = [sessionWrapper](...)
    {
        m_sessionPool.release(sessionWrapper);

        if (!m_queue.empty())
        {
            DatabaseManager::Task nextTask = m_queue.front();
            m_queue.pop();
            throwQuery(std::move(nextTask));
        }
    };

    ThreadPool::addTask(std::move(asyncTask));
}

// Not thread safe, should be called from the main thread
void DatabaseManager::selectQuery(DatabaseManager::SelectTask task, DatabaseManager::SelectCallback callback)
{
    SessionWrapper *sessionWrapper = m_sessionPool.get();
    if (sessionWrapper == nullptr)
    {
        m_selectQueue.push({task, callback});
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
        m_sessionPool.release(sessionWrapper);

        if (!m_selectQueue.empty())
        {
            auto [nextTask, nextCallback] = m_selectQueue.front();
            m_selectQueue.pop();
            selectQuery(std::move(nextTask), std::move(nextCallback));
        }
    };

    ThreadPool::addTask(std::move(asyncTask));
}
