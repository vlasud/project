#pragma once

#include "types.hpp"
#include <algorithm>
#include <cmath>

namespace Utils
{
// Анти-краш санация числовых значений от клиента: спуфнутый синк может прислать
// NaN/Inf в координатах/размерах, на которых GTA-движок крашится или уводит
// объект в бесконечность. Нефинитное заменяем на 0, финитное пропускаем как есть.
inline float finiteOrZero(float value)
{
    return std::isfinite(value) ? value : 0.0f;
}

// Пер-компонентная санация вектора позиции/поворота от нефинитных значений.
inline Vector3 sanitize(Vector3 v)
{
    return {finiteOrZero(v.x), finiteOrZero(v.y), finiteOrZero(v.z)};
}

// Санация + зажим в [min, max]: нефинитное сначала обнуляется, затем клампится.
inline float clampFinite(float value, float min, float max)
{
    return std::clamp(finiteOrZero(value), min, max);
}
} // namespace Utils
