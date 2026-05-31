#include "DatabaseManager.h"

#include "../Log/LogManager.h"
#include "../ThreadPool/ThreadPool.h"
#include "core.hpp"
#include "fmt/base.h"
#include <memory>

namespace
{
const char *HOST = "tcp://127.0.0.1:3306";
const char *USER = "root";
const char *PASSWORD = "root";
const char *DATABASE = "test";
} // namespace

bool ConnectionWrapper::initialize()
{
    m_driver = std::unique_ptr<sql::mysql::MySQL_Driver>(sql::mysql::get_mysql_driver_instance());
    if (!m_driver)
    {
        LogManager::log(LogLevel::Error, "Failed to get MySQL driver instance.");
        return false;
    }

    m_connection = std::unique_ptr<sql::Connection>(m_driver->connect(HOST, USER, PASSWORD));
    if (!m_connection || !m_connection->isValid())
    {
        LogManager::log(LogLevel::Error, "Failed to establish database connection.");
        return false;
    }

    m_connection->setSchema(DATABASE);
    return true;
}

const std::unique_ptr<sql::Connection> &ConnectionWrapper::getConnection() const
{
    return m_connection;
}

void DatabaseManager::initialize()
{
    bool allConnectionsInitialized = false;
    m_connectionPool.forEach(
        [&allConnectionsInitialized](auto *connectionWrapper)
        {
            allConnectionsInitialized = connectionWrapper->initialize();
            return allConnectionsInitialized;
        });

    if (!allConnectionsInitialized)
    {
        LogManager::log(LogLevel::Error, "Failed to initialize all database connections.");
        return;
    }

    char buffer[256] = {0};
    fmt::format_to_n(buffer, sizeof(buffer), "DatabaseManager initialized with connection pool size: {}",
                     m_connectionPool.size());
    LogManager::log(LogLevel::Message, buffer);
}

void DatabaseManager::query(std::function<void()> task)
{
    ThreadPool::Task queryTask;

    queryTask.asyncFunc = []()
    {
        ConnectionWrapper *connectionWrapper = m_connectionPool.get();
        std::unique_ptr<sql::PreparedStatement> pstmt(
            connectionWrapper->getConnection()->prepareStatement("INSERT INTO test (name) VALUES (?)"));
        pstmt->setString(1, "Ivan_Petrov");
        pstmt->executeUpdate();
    };

    queryTask.resultCallback = [connectionWrapper]() { m_connectionPool.release(connectionWrapper); };
    ThreadPool::addTaskWithResult(std::move(queryTask));
}
