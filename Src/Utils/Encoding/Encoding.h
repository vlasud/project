#pragma once

#include <string>

class Encoding final
{
  public:
    static std::string utf8Tocp1251(std::string_view input);
};