#pragma once

#include <cstdint>
#include <string>

// Деньги для игроцких окон. Крупные суммы без разделителя читаются плохо:
// «100000» и «1000000» отличаются одним символом, а на аукционе от этого зависит
// ставка. Разряды делим ТОЧКОЙ (10.000$) — так их задал владелец.
//
// Знак идёт ПОСЛЕ числа: «100.000$». Внутри проекта много мест с префиксным «$100»
// — их не трогаем, здесь формат аукционных окон.
namespace Money
{
// «100.000$». Отрицательные (в аукционе их не бывает) печатаются со знаком минус.
inline std::string text(std::int64_t amount)
{
    const bool negative = amount < 0;
    // Модуль берём в unsigned: у INT64_MIN нет положительной пары, и -amount на нём
    // был бы UB.
    const auto magnitude = negative ? 0ULL - static_cast<std::uint64_t>(amount) : static_cast<std::uint64_t>(amount);

    const std::string digits = std::to_string(magnitude);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3 + 2);
    if (negative)
    {
        out.push_back('-');
    }
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        // Считаем ОТ КОНЦА: точка перед каждой полной тройкой хвоста. Счёт от
        // начала («i минус длина первой группы») на size_t уходил в минус и
        // переполнялся, а SIZE_MAX кратен трём — вылезала лишняя точка (5.0.000$).
        if (i != 0 && (digits.size() - i) % 3 == 0)
        {
            out.push_back('.');
        }
        out.push_back(digits[i]);
    }
    out.push_back('$');
    return out;
}
} // namespace Money
