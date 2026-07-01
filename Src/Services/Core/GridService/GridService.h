#pragma once

#include "Services/IService.h"
#include "types.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// Тип сущности в сетке. Значение используется как номер бита в маске запроса.
enum class GridEntityType : std::uint8_t
{
    Player = 0,
    Vehicle,
    Object,
    Pickup,
    MapIcon,
    Actor,
    TextLabel,
};

constexpr std::uint32_t gridMask(GridEntityType type)
{
    return 1u << static_cast<std::uint8_t>(type);
}
constexpr std::uint32_t GRID_MASK_ALL = ~0u;

// Пространственная сетка (spatial hash по равномерным ячейкам) — фундамент
// стриминга. Мир разбит на ячейки CELL_SIZE×CELL_SIZE по X/Y; каждая ячейка
// хранит плоский массив сущностей с координатами inline — запрос по радиусу
// это линейный проход по нескольким соседним ячейкам без разыменований.
//
// Производительность:
//  * add/remove/move — O(1): swap-and-pop в ячейке, слоты дают обратный адрес;
//  * move в пределах той же ячейки (типичный случай для игроков) — только
//    перезапись координат, без перестроений;
//  * запросы не аллоцируют: forEachInRadius зовёт колбэк, queryRadius пишет
//    в буфер вызывающего (переиспользуй его между кадрами);
//  * фильтр по типам — битовая маска: gridMask(Player) | gridMask(Vehicle).
//
// Игроков и машины ведёт GridSystem; статику (объекты, пикапы, иконки)
// регистрируют их будущие системы через add/move/remove, храня Handle.
class GridService final : public IService
{
  public:
    using Handle = std::int32_t;
    static constexpr Handle INVALID_HANDLE = -1;

    struct Result
    {
        GridEntityType type;
        std::int32_t id; // pool id игрока/машины или id, заданный при add()
        float distSq;    // квадрат расстояния до центра запроса
    };

    GridService();

    // --- отладка/инспекция ---
    struct CellCoords
    {
        int cx;
        int cy;
    };
    static CellCoords cellOf(const Vector3 &position)
    {
        return {cellCoord(position.x), cellCoord(position.y)};
    }
    static constexpr float cellSize()
    {
        return CELL_SIZE;
    }

    // --- ведение сущностей ---
    Handle add(GridEntityType type, std::int32_t id, const Vector3 &position);
    void move(Handle handle, const Vector3 &position);
    void remove(Handle handle);

    // --- запросы ---
    // Зовёт visit(GridEntityType, int32 id, float distSq) для каждой сущности в
    // радиусе (3D-расстояние). Самый быстрый путь — ноль аллокаций и копий.
    template <typename F> void forEachInRadius(const Vector3 &center, float radius, std::uint32_t typeMask, F &&visit) const
    {
        const float radiusSq = radius * radius;
        const int cx0 = cellCoord(center.x - radius);
        const int cx1 = cellCoord(center.x + radius);
        const int cy0 = cellCoord(center.y - radius);
        const int cy1 = cellCoord(center.y + radius);

        for (int cy = cy0; cy <= cy1; ++cy)
        {
            const int rowBase = cy * COLS;
            for (int cx = cx0; cx <= cx1; ++cx)
            {
                for (const Entry &entry : m_cells[rowBase + cx])
                {
                    if (!(typeMask & (1u << static_cast<std::uint8_t>(entry.type))))
                        continue;
                    const float dx = entry.x - center.x;
                    const float dy = entry.y - center.y;
                    const float dz = entry.z - center.z;
                    const float distSq = dx * dx + dy * dy + dz * dz;
                    if (distSq <= radiusSq)
                        visit(entry.type, entry.id, distSq);
                }
            }
        }
    }

    // Заполняет буфер вызывающего (clear + push). Возвращает число найденных.
    std::size_t queryRadius(const Vector3 &center, float radius, std::uint32_t typeMask, std::vector<Result> &out) const;

    // Ближайшая сущность в радиусе. false — никого нет.
    bool closest(const Vector3 &center, float radius, std::uint32_t typeMask, Result &out) const;

  private:
    // Элемент ячейки: координаты и идентификация inline — проход по ячейке
    // не трогает ничего, кроме её собственного непрерывного массива.
    struct Entry
    {
        float x, y, z;
        std::int32_t id;
        Handle slot; // обратный адрес для O(1) обновления при swap-and-pop
        GridEntityType type;
    };

    // Слот — стабильный адрес сущности: в какой ячейке и на какой позиции лежит.
    struct Slot
    {
        std::int32_t cell = -1; // -1 — слот свободен
        std::int32_t indexInCell = -1;
    };

    // Мир SA: ±3000 по X/Y, берём с запасом под объекты за границей.
    static constexpr float WORLD_MIN = -3200.0f;
    static constexpr float WORLD_MAX = 3200.0f;
    static constexpr float CELL_SIZE = 128.0f;
    static constexpr float INV_CELL_SIZE = 1.0f / CELL_SIZE;
    static constexpr int COLS = static_cast<int>((WORLD_MAX - WORLD_MIN) * INV_CELL_SIZE); // 50
    static constexpr int CELL_COUNT = COLS * COLS;

    // Координата → индекс ячейки по оси; всё вне мира прижимается к крайним.
    // Не-конечную координату (NaN/Inf — клиент может прислать её в позиции игрока
    // ИЛИ машины из синка) трактуем как 0-ю ячейку: static_cast<int> от NaN/Inf —
    // UB, а кламп ниже NaN не ловит (сравнения с NaN ложны). Гард у общего стока.
    static int cellCoord(float v)
    {
        if (!std::isfinite(v))
            return 0;
        const int c = static_cast<int>((v - WORLD_MIN) * INV_CELL_SIZE);
        return c < 0 ? 0 : (c >= COLS ? COLS - 1 : c);
    }
    static int cellIndex(float x, float y)
    {
        return cellCoord(y) * COLS + cellCoord(x);
    }

    bool validHandle(Handle handle) const
    {
        return handle >= 0 && handle < static_cast<Handle>(m_slots.size()) && m_slots[handle].cell >= 0;
    }

    // Убирает элемент из ячейки swap-and-pop'ом, поправляя слот переехавшего.
    void removeFromCell(std::int32_t cellIdx, std::int32_t indexInCell);

    std::array<std::vector<Entry>, CELL_COUNT> m_cells;
    std::vector<Slot> m_slots;
    std::vector<Handle> m_freeSlots;
};
