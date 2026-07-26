#include "Services/Core/PlayerCommandService/PlayerCommandService.h"

#include "Utils/Encoding/Encoding.h"
#include "fmt/format.h"
#include <algorithm>
#include <charconv>

namespace
{
// Корневой антифлуд (решение геймдизайнера). «Та же команда» — полная строка
// после '/' (имя+параметры) целиком, регистронезависимо по ASCII; пробелы не
// нормализуются.
constexpr int CMD_FLOOD_THRESHOLD = 5;                       // блок ставит 5-й одинаковый ввод подряд
constexpr std::chrono::milliseconds CMD_FLOOD_WINDOW{1500};  // макс. интервал между повторами
constexpr std::chrono::milliseconds CMD_FLOOD_BLOCK{3000};   // длительность блока всех команд
constexpr std::chrono::milliseconds CMD_FLOOD_NOTICE_COOLDOWN{1000}; // мин. интервал показа сообщения о блоке

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
    //
    // Считаем строго в uint64: на 32-битной сборке (наша) size_t — 32 бита, и
    // 64-битные константы FNV молча усекались, давая не FNV-1a, а произвольный
    // его обрезок. Сужаем только результат.
    std::uint64_t h = 14695981039346656037ull;
    for (char c : s)
    {
        h ^= static_cast<unsigned char>(asciiLower(c));
        h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
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

void PlayerCommandService::add(std::string name, std::vector<Param> params, Handler handler, PermissionSpec perm,
                               std::string description, HelpCategory helpCategory)
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

    // description хранится как utf-8 (НЕ кодируем): он попадает в тело диалога
    // /help|/ahelp и кодируется один раз вместе со всем телом.
    m_commands.emplace(std::move(name), Command{std::move(infos), Encoding::utf8Tocp1251(usage), std::move(handler),
                                                perm, std::move(description), helpCategory});
}

void PlayerCommandService::setPermissionResolver(PermissionResolver resolver)
{
    m_permissionResolver = std::move(resolver);
}

std::vector<PlayerCommandService::AccessibleCommand> PlayerCommandService::collectAccessible(IPlayer &player) const
{
    std::vector<AccessibleCommand> result;
    // Только AdminLevel-команды (админ- и dev-тулинг); фильтр — тем же резолвером,
    // что и dispatch, по эффективному уровню игрока. Резолвер не задан → ни одна
    // команда с порогом недоступна (как в dispatch). Имя возвращаем как хранится
    // (нижний регистр).
    for (const auto &[name, cmd] : m_commands)
    {
        if (cmd.perm.kind != PermissionSpec::Kind::AdminLevel)
            continue;
        if (m_permissionResolver && m_permissionResolver(player, cmd.perm))
            result.push_back({name, cmd.perm.adminLevel, cmd.description});
    }
    return result;
}

std::vector<PlayerCommandService::HelpCommand> PlayerCommandService::collectHelpCommands() const
{
    std::vector<HelpCommand> result;
    // Все команды с заявленной категорией (Hidden исключаем — это /ahelp).
    // Право тут НЕ проверяем: фракционные команды зарегистрированы как Kind::None
    // (право внутри обработчика), поэтому охват /help задаётся категорией + членством
    // во фракции (членство проверяет вызывающий). Имя — как хранится (нижний регистр).
    for (const auto &[name, cmd] : m_commands)
    {
        if (cmd.helpCategory == HelpCategory::Hidden)
            continue;
        result.push_back({name, cmd.description, cmd.helpCategory});
    }
    return result;
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

    // Гейт прав — ДО антифлуда и разбора аргументов. Отказ возвращает false (как
    // будто команды нет): вызывающая система покажет «неизвестная команда»,
    // скрывая само существование недоступной команды (защита от прощупывания).
    // Стоит ПЕРЕД антифлудом намеренно: иначе блок-сообщение по недоступной
    // команде выдало бы её существование. Резолвер не задан и команда с порогом —
    // тоже отказ (открыто только то, что явно разрешено). Спек None всегда проходит.
    if (cmd.perm.kind != PermissionSpec::Kind::None &&
        !(m_permissionResolver && m_permissionResolver(player, cmd.perm)))
        return false;

    // Корневой антифлуд — ДО разбора аргументов/usage, чтобы ловить и спам
    // usage-сообщений. «Та же команда» = вся строка после '/' (имя+параметры),
    // ASCII-регистронезависимо; нормализуем в стековый буфер (без аллокаций).
    const TimePoint now = std::chrono::steady_clock::now();
    {
        const StringView rawLine(message.data() + 1, message.size() - 1);
        char lowerBuf[LAST_COMMAND_CAP];
        const std::size_t lowerLen = std::min<std::size_t>(rawLine.size(), LAST_COMMAND_CAP);
        for (std::size_t i = 0; i < lowerLen; ++i)
            lowerBuf[i] = asciiLower(rawLine[i]);
        if (isFlooding(player, StringView(lowerBuf, lowerLen), now))
            return true;
    }

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

bool PlayerCommandService::isFlooding(IPlayer &player, StringView normalizedLine, TimePoint now)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    State &state = m_state[playerId];

    // Активный блок: все команды отклоняются. Сообщение об остатке троттлим.
    if (now < state.blockUntil)
    {
        if (now - state.lastNoticeAt >= CMD_FLOOD_NOTICE_COOLDOWN)
        {
            const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(state.blockUntil - now);
            // ceil остатка в секундах (целые секунды → как есть).
            const long long secondsLeft = (remainingMs.count() + 999) / 1000;
            player.sendClientMessage(Colour::White(),
                                     u(fmt::format("Команды заблокированы. Осталось: {} сек.", secondsLeft)));
            state.lastNoticeAt = now;
        }
        return true;
    }

    // Счётчик одинаковых вводов в окне. Сравниваем нормализованную строку с
    // прошлой; разрыв больше окна — счётчик сбрасывается.
    const StringView lastCommand(state.lastCommand.data(), state.lastCommandLen);
    if (normalizedLine == lastCommand && now - state.lastCommandAt < CMD_FLOOD_WINDOW)
        ++state.repeatCount;
    else
        state.repeatCount = 1;

    state.lastCommandLen = std::min<std::size_t>(normalizedLine.size(), LAST_COMMAND_CAP);
    std::copy_n(normalizedLine.data(), state.lastCommandLen, state.lastCommand.data());
    state.lastCommandAt = now;

    if (state.repeatCount >= CMD_FLOOD_THRESHOLD)
    {
        state.blockUntil = now + CMD_FLOOD_BLOCK;
        state.repeatCount = 0;
        const long long blockSeconds = std::chrono::duration_cast<std::chrono::seconds>(CMD_FLOOD_BLOCK).count();
        player.sendClientMessage(
            Colour::White(),
            u(fmt::format("Слишком много одинаковых команд. Подождите {} сек.", blockSeconds)));
        state.lastNoticeAt = now;
        return true;
    }

    return false;
}

void PlayerCommandService::reset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_state[playerId] = State{};
}
