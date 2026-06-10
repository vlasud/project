#include "Log/LogManager.h"

void LogManager::initialize(ICore *core)
{
    m_core = core;
}

void LogManager::log(LogLevel level, const std::string &message)
{
    if (m_core)
    {
        m_core->logLn(level, "%s", message.c_str());
    }
}
