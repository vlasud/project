#pragma once

#include "../Pools/StaticPool.h"
#include <functional>
#include <mysqlx/xdevapi.h>
#include <queue>

class SessionWrapper
{
  public:
    bool initialize();
    mysqlx::Schema getSchema();

  private:
    std::unique_ptr<mysqlx::Session> m_session;
};

class DatabaseManager
{
    using Task = std::function<void(mysqlx::Schema)>;
    using SelectTask = std::function<mysqlx::RowResult(mysqlx::Schema)>;
    using SelectCallback = std::function<void(mysqlx::RowResult)>;
    using ErrorCallback = std::function<void(const std::string &)>;

  public:
    static void initialize();
    static void flush();
    // errorCallback (опционально) вызывается на главном потоке, если запрос бросил
    // исключение. Сессия в любом случае возвращается в пул, ошибка логируется.
    static void throwQuery(DatabaseManager::Task task, DatabaseManager::ErrorCallback errorCallback = {});
    static void selectQuery(DatabaseManager::SelectTask task, DatabaseManager::SelectCallback callback,
                            DatabaseManager::ErrorCallback errorCallback = {});
    static mysqlx::Schema getSchema();

  private:
    struct PendingThrow
    {
        Task task;
        ErrorCallback errorCallback;
    };
    struct PendingSelect
    {
        SelectTask task;
        SelectCallback callback;
        ErrorCallback errorCallback;
    };

    static void releaseAndPump(SessionWrapper *sessionWrapper);

    inline static StaticPool<SessionWrapper, 16> m_sessionPool;
    inline static std::queue<PendingThrow> m_queue;
    inline static std::queue<PendingSelect> m_selectQueue;
};
