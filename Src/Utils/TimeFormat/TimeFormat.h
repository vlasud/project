#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <fmt/format.h>
#include <string>

// Человекочитаемое время для игроцких окон: абсолютный момент и «сколько
// осталось». Единый источник формата — иначе одна и та же дата в разных диалогах
// выглядела бы по-разному (аукцион бизнеса и /auc показывают её бок о бок).
//
// Время АБСОЛЮТНОЕ (unix): сроки, которые обязаны идти и пока сервер лежит,
// хранятся именно так, а не steady_clock-таймером в памяти.
namespace TimeFormat
{
// Сейчас, в unix-секундах.
inline std::int64_t nowUnix()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

// Абсолютный момент в локальную дату сервера: «28.07.2026 21:15».
// localtime_s, а не localtime: последний отдаёт указатель на общий статический
// буфер, и параллельный вызов с воркера БД перетёр бы результат под руками.
inline std::string dateTime(std::int64_t unixTime)
{
    const auto stamp = static_cast<std::time_t>(unixTime);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &stamp) != 0)
    {
        return "—";
    }
#else
    if (localtime_r(&stamp, &local) == nullptr)
    {
        return "—";
    }
#endif
    return fmt::format("{:02}.{:02}.{} {:02}:{:02}", local.tm_mday, local.tm_mon + 1, local.tm_year + 1900,
                       local.tm_hour, local.tm_min);
}

// Остаток в крупных единицах: «1 д 4 ч» / «3 ч 20 мин» / «7 мин». Мелочь ниже
// минуты игроку не нужна — сроки тут суточные.
inline std::string left(std::int64_t seconds)
{
    if (seconds <= 0)
    {
        return "меньше минуты";
    }
    const std::int64_t days = seconds / 86400;
    const std::int64_t hours = (seconds % 86400) / 3600;
    const std::int64_t minutes = (seconds % 3600) / 60;
    if (days > 0)
    {
        return fmt::format("{} д {} ч", days, hours);
    }
    if (hours > 0)
    {
        return fmt::format("{} ч {} мин", hours, minutes);
    }
    return fmt::format("{} мин", std::max<std::int64_t>(minutes, 1));
}
} // namespace TimeFormat
