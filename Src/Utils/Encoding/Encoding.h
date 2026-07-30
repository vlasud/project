#pragma once

#include <cstddef>
#include <string>
#include <string_view>

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

    // Чистка ГОТОВОЙ строки, уходящей клиенту, НА МЕСТЕ: цветокоды ('{', '}', '~')
    // и управляющие байты (< 0x20 и 0x7F, они рвут client message). Строка к этому
    // моменту уже в cp1251 — кодировка однобайтовая, все заменяемые символы ASCII,
    // кириллицу не задеваем.
    //
    // Отдельно от neutralizeColorCodes: тот работает с utf-8 и возвращает копию, а
    // строки чата собираются в стековый буфер и правятся без аллокаций.
    static void neutralizeLine(char *data, std::size_t size);
};

// UTF-8 (кодировка исходника) -> CP1251 (кодировка рендера кириллицы клиентом SA-MP/open.mp).
// Русский текст диалогов/чата/текстдравов ОБЯЗАН пройти через это, иначе на клиенте кракозябры.
// string_view принимает и строковые литералы, и std::string, и результат fmt::format без лишней аллокации.
inline std::string u(std::string_view text)
{
    return Encoding::utf8Tocp1251(text);
}