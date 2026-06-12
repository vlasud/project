#pragma once

#include <string>

class Encoding final
{
  public:
    static std::string utf8Tocp1251(std::string_view input);
    // Обратное преобразование — для клиентского ввода (диалоги, чат), уходящего
    // в БД/логи: внутреннее и хранимое представление текста — utf-8.
    static std::string cp1251Toutf8(std::string_view input);
};