#pragma once

#include "../Pools/StaticPool.h"
#include <jdbc.h>

class ConnectionWrapper
{
  public:
    bool initialize();
    const std::unique_ptr<sql::Connection> &getConnection() const;

  private:
    std::unique_ptr<sql::mysql::MySQL_Driver> m_driver;
    std::unique_ptr<sql::Connection> m_connection;
};

class DatabaseManager
{
  public:
    static void initialize();
    static void query(std::function<void()> task);

  private:
    inline static StaticPool<ConnectionWrapper, 10> m_connectionPool;
};
