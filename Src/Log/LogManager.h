#pragma once

#include "core.hpp"
#include <string>

class LogManager
{
  public:
    static void initialize(ICore *core);
    static void log(LogLevel level, const std::string &message);

  private:
    inline static ICore *m_core = nullptr;
};
