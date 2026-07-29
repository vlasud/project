#pragma once

#include "types.hpp"
#include <cmath>

// Геометрия направления по углу поворота (yaw) в SA-MP конвенции. Единый источник
// формулы «вперёд/назад» — вызыватели (HouseService::backOf, ParkingSystem пробы
// мест) считают её через этот хелпер, без локальных копий PI/sin/cos.
//
// Конвенция SA-MP: угол a в РАДИАНАХ, «вперёд» = (-sin(a), cos(a), 0), значит
// «назад» = (sin(a), -cos(a), 0). Аргумент angleDegrees — в ГРАДУСАХ; перевод в
// радианы и константа PI живут ВНУТРИ (тот же множитель и то же значение, что были
// в вызывателях — результат для любого угла численно неизменен).
namespace Geometry
{
// Значение PI как в прежних локальных копиях (HouseService/ParkingSystem) — иначе
// сдвинулись бы младшие биты тригонометрии для ненулевых углов.
inline constexpr float PI = 3.14159265358979323846f;

// Округлить угол до ближайшей ЧЕТВЕРТИ ОБОРОТА: 0 / 90 / 180 / 270 (360 — это 0).
//
// Зачем: угол берётся из клиентского поворота игрока и приходит сырым (187.34,
// 359.71). По такому углу точка «за спиной» уезжает с оси мира — на глаз объект
// оказывается повёрнут и смещён «под небольшим углом». Округление даёт ровные
// оси и повторяемый результат: два дома, поставленные примерно одинаково, встанут
// одинаково. Мусор (NaN/inf) -> 0.
inline float snapToQuarterTurn(float angleDegrees)
{
    if (!std::isfinite(angleDegrees))
    {
        return 0.0f;
    }
    const float snapped = std::fmod(std::round(angleDegrees / 90.0f) * 90.0f, 360.0f);
    return snapped < 0.0f ? snapped + 360.0f : snapped; // отрицательные углы -> [0, 360)
}

// Единичный вектор «вперёд» по углу (yaw, градусы): (-sin(a), cos(a), 0), a в рад.
inline Vector3 headingVector(float angleDegrees)
{
    const float a = angleDegrees * PI / 180.0f;
    return {-std::sin(a), std::cos(a), 0.0f};
}

// Точка в dist метрах «вперёд» от pos по углу: pos + dist * headingVector(angle).
// = {pos.x - sin(a)*dist, pos.y + cos(a)*dist, pos.z}.
inline Vector3 forwardOf(const Vector3 &pos, float angleDegrees, float dist)
{
    const float a = angleDegrees * PI / 180.0f;
    return {pos.x - std::sin(a) * dist, pos.y + std::cos(a) * dist, pos.z};
}

// Точка в dist метрах «назад» от pos по углу: pos - dist * headingVector(angle).
// = {pos.x + sin(a)*dist, pos.y - cos(a)*dist, pos.z}.
inline Vector3 backOf(const Vector3 &pos, float angleDegrees, float dist)
{
    const float a = angleDegrees * PI / 180.0f;
    return {pos.x + std::sin(a) * dist, pos.y - std::cos(a) * dist, pos.z};
}
} // namespace Geometry
