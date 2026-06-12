#pragma once

#include <string>

class Encoding final
{
  public:
    static std::string utf8Tocp1251(std::string_view input);
    // Обратное преобразование — для клиентского ввода (диалоги, чат), уходящего
    // в БД/логи: внутреннее и хранимое представление текста — utf-8.
    static std::string cp1251Toutf8(std::string_view input);

    // Чистка клиентского utf-8 текста: управляющие символы (включая \t —
    // разделитель tablist-диалогов) выбрасываются, края обрезаются от пробелов,
    // длина ограничивается maxBytes БЕЗ разрыва utf-8 символа (включая ведущий
    // байт разрезанной последовательности — иначе в cp1251 вылезают '?').
    static std::string sanitizeUserText(std::string_view utf8, std::size_t maxBytes);
};