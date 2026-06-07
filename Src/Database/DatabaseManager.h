#pragma once

#include "../Pools/StaticPool.h"
#include <functional>
#include <mysqlx/xdevapi.h>
#include <queue>

class SessionWrapper
{
  public:
    void initialize();
    mysqlx::Schema getSchema();

  private:
    std::unique_ptr<mysqlx::Session> m_session;
};

class DatabaseManager
{
    using Task = std::function<void(mysqlx::Schema)>;
    using SelectTask = std::function<mysqlx::RowResult(mysqlx::Schema)>;
    using SelectCallback = std::function<void(mysqlx::RowResult)>;

  public:
    static void initialize();
    static void flush();
    static void throwQuery(DatabaseManager::Task task);
    static void selectQuery(DatabaseManager::SelectTask task, DatabaseManager::SelectCallback callback);
    static mysqlx::Schema getSchema();

  private:
    inline static StaticPool<SessionWrapper, 16> m_sessionPool;
    inline static std::queue<DatabaseManager::Task> m_queue;
    inline static std::queue<std::pair<DatabaseManager::SelectTask, DatabaseManager::SelectCallback>> m_selectQueue;
};
