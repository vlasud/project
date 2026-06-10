#include "Services/Core/PlayerCommandService/PlayerCommandService.h"

#include "Utils/Encoding/Encoding.h"
#include <charconv>

namespace
{
// ASCII-нижний регистр: без локали и без UB на отрицательных char. Имён команд
// в кириллице мы не ждём, поэтому ASCII достаточно.
inline char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// Целое только если распарсился весь токен целиком: "12abc" и "12 3" — не число.
bool parseIntFull(StringView token, int &out)
{
    const char *begin = token.data();
    const char *end = begin + token.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc() && ptr == end;
}
} // namespace

std::size_t PlayerCommandService::CiHash::operator()(StringView s) const noexcept
{
    // FNV-1a по нижнему регистру: хэш не зависит от регистра ввода.
    std::size_t h = 14695981039346656037ull;
    for (char c : s)
    {
        h ^= static_cast<unsigned char>(asciiLower(c));
        h *= 1099511628211ull;
    }
    return h;
}

bool PlayerCommandService::CiEqual::operator()(StringView a, StringView b) const noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (asciiLower(a[i]) != asciiLower(b[i]))
            return false;
    return true;
}

void PlayerCommandService::add(std::string name, std::vector<Param> params, Handler handler)
{
    for (char &c : name)
        c = asciiLower(c);

    // Подсказку и сообщения об ошибках собираем один раз здесь и сразу кодируем —
    // в dispatch остаётся только отправить готовую строку.
    std::string usage = "Использование: /" + name;
    std::vector<ParamInfo> infos;
    infos.reserve(params.size());
    for (const Param &param : params)
    {
        usage += " [" + param.name + "]";

        std::string numberError;
        if (param.type == Param::Int)
            numberError = Encoding::utf8Tocp1251("Параметр [" + param.name + "] должен быть целым числом");
        infos.push_back({param.type, std::move(numberError)});
    }

    m_commands.emplace(std::move(name), Command{std::move(infos), Encoding::utf8Tocp1251(usage), std::move(handler)});
}

bool PlayerCommandService::dispatch(IPlayer &player, StringView message)
{
    if (message.size() < 2 || message[0] != '/')
        return false;

    const char *p = message.data() + 1; // пропускаем '/'
    const char *end = message.data() + message.size();

    // Имя команды — до первого пробела.
    const char *nameBegin = p;
    while (p < end && *p != ' ')
        ++p;
    StringView name(nameBegin, static_cast<std::size_t>(p - nameBegin));

    auto it = m_commands.find(name); // гетерогенный поиск, без аллокации ключа
    if (it == m_commands.end())
        return false;

    const Command &cmd = it->second;
    const std::size_t paramCount = cmd.params.size();

    // Начало области аргументов.
    while (p < end && *p == ' ')
        ++p;

    std::vector<Arg> args;
    args.reserve(paramCount);

    // Все аргументы, кроме последнего, — токены без пробелов.
    while (args.size() + 1 < paramCount && p < end)
    {
        const char *argBegin = p;
        while (p < end && *p != ' ')
            ++p;
        args.push_back({StringView(argBegin, static_cast<std::size_t>(p - argBegin)), 0});
        while (p < end && *p == ' ')
            ++p;
    }

    // Последний аргумент — жадный: весь остаток без хвостовых пробелов.
    if (paramCount > 0 && p < end)
    {
        const char *tail = end;
        while (tail > p && *(tail - 1) == ' ')
            --tail;
        if (tail > p)
            args.push_back({StringView(p, static_cast<std::size_t>(tail - p)), 0});
    }

    if (args.size() < paramCount)
    {
        player.sendClientMessage(Colour::White(), cmd.usage);
        return true;
    }

    // Проверка и парсинг типов: числовые параметры превращаем в int здесь, чтобы
    // обработчик получил готовое значение.
    for (std::size_t i = 0; i < paramCount; ++i)
    {
        if (cmd.params[i].type == Param::Int && !parseIntFull(args[i].text, args[i].number))
        {
            player.sendClientMessage(Colour::White(), cmd.params[i].numberError);
            return true;
        }
    }

    cmd.handler(player, CommandArgs(args));
    return true;
}
