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

    // Обезвреживает символы цветокодов клиента в utf-8 строке: '{'->'(', '}'->')',
    // '~'->'-'. Клиент SA-MP/open.mp парсит {RRGGBB} и ~r~-коды в client message
    // при рендере независимо от сервера — иначе игрок подделает цвет/перебьёт
    // строку. Все три символа однобайтовые ASCII (< 0x80), continuation-байты
    // utf-8 (>= 0x80) не задевают — порча мультибайтных символов невозможна.
    static std::string neutralizeColorCodes(std::string_view utf8);
};