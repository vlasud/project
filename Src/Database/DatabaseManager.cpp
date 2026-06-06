#include "DatabaseManager.h"

#include "../Log/LogManager.h"
#include "../ThreadPool/ThreadPool.h"
#include "core.hpp"
#include "mysqlx/devapi/settings.h"
#include <memory>

namespace
{
const std::string HOST = "127.0.0.1";
const std::string USER = "root";
const std::string PASSWORD = "root";
const std::string DATABASE = "test";
const std::string PORT = "33060";
} // namespace

void SessionWrapper::initialize()
{
    std::string uri = "mysqlx://" + USER + ":" + PASSWORD + "@" + HOST + ":" + PORT + "/" + DATABASE;
    m_session = std::make_unique<mysqlx::Session>(uri);
}

mysqlx::Schema SessionWrapper::getSchema()
{
    return m_session->getSchema(DATABASE);
}

void SessionWrapper::setRowResult(mysqlx::RowResult result)
{
    m_result = std::move(result);
}

mysqlx::RowResult &&SessionWrapper::moveOutRowResult()
{
    return std::move(m_result);
}

void DatabaseManager::initialize()
{
    m_sessionPool.forEach(
        [](SessionWrapper &wrapper)
        {
            wrapper.initialize();
            return true;
        });

    LogManager::log(Message, "DatabaseManager initialized with " + std::to_string(m_sessionPool.size()) + " sessions");
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

    ThreadPool::Task asyncTask;

    asyncTask.func = [task = std::move(task), sessionWrapper]()
    {
        task(sessionWrapper->getSchema());
    };

    asyncTask.callback = [sessionWrapper]()
    {
        m_sessionPool.release(sessionWrapper);

        if (!m_queue.empty())
        {
            DatabaseManager::Task nextTask = m_queue.front();
            m_queue.pop();
            throwQuery(std::move(nextTask));
        }
    };

    ThreadPool::addTaskWithCallback(std::move(asyncTask));
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

    ThreadPool::Task asyncTask;

    asyncTask.func = [task = std::move(task), sessionWrapper]()
    {
        sessionWrapper->setRowResult(task(sessionWrapper->getSchema()));
    };

    asyncTask.callback = [sessionWrapper, callback = std::move(callback)]()
    {
        callback(sessionWrapper->moveOutRowResult());
        m_sessionPool.release(sessionWrapper);

        if (!m_selectQueue.empty())
        {
            auto [nextTask, nextCallback] = m_selectQueue.front();
            m_selectQueue.pop();
            selectQuery(std::move(nextTask), std::move(nextCallback));
        }
    };

    ThreadPool::addTaskWithCallback(std::move(asyncTask));
}
