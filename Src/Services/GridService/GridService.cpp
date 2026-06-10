#include "GridService.h"

GridService::GridService()
{
    m_slots.reserve(4096);
    m_freeSlots.reserve(256);
}

GridService::Handle GridService::add(GridEntityType type, std::int32_t id, const Vector3 &position)
{
    Handle handle;
    if (!m_freeSlots.empty())
    {
        handle = m_freeSlots.back();
        m_freeSlots.pop_back();
    }
    else
    {
        handle = static_cast<Handle>(m_slots.size());
        m_slots.emplace_back();
    }

    const int cellIdx = cellIndex(position.x, position.y);
    std::vector<Entry> &cell = m_cells[cellIdx];

    m_slots[handle] = {cellIdx, static_cast<std::int32_t>(cell.size())};
    cell.push_back({position.x, position.y, position.z, id, handle, type});
    return handle;
}

void GridService::move(Handle handle, const Vector3 &position)
{
    if (!validHandle(handle))
        return;

    Slot &slot = m_slots[handle];
    const int cellIdx = cellIndex(position.x, position.y);

    if (cellIdx == slot.cell)
    {
        // Типичный случай: сущность осталась в своей ячейке — только координаты.
        Entry &entry = m_cells[slot.cell][slot.indexInCell];
        entry.x = position.x;
        entry.y = position.y;
        entry.z = position.z;
        return;
    }

    // Переезд между ячейками: копия наружу, swap-and-pop, вставка в новую.
    Entry entry = m_cells[slot.cell][slot.indexInCell];
    removeFromCell(slot.cell, slot.indexInCell);

    entry.x = position.x;
    entry.y = position.y;
    entry.z = position.z;

    std::vector<Entry> &cell = m_cells[cellIdx];
    slot.cell = cellIdx;
    slot.indexInCell = static_cast<std::int32_t>(cell.size());
    cell.push_back(entry);
}

void GridService::remove(Handle handle)
{
    if (!validHandle(handle))
        return;

    Slot &slot = m_slots[handle];
    removeFromCell(slot.cell, slot.indexInCell);
    slot.cell = -1;
    slot.indexInCell = -1;
    m_freeSlots.push_back(handle);
}

void GridService::removeFromCell(std::int32_t cellIdx, std::int32_t indexInCell)
{
    std::vector<Entry> &cell = m_cells[cellIdx];
    const Entry &last = cell.back();

    if (indexInCell != static_cast<std::int32_t>(cell.size()) - 1)
    {
        cell[indexInCell] = last;
        m_slots[last.slot].indexInCell = indexInCell; // переехавшему — новый адрес
    }
    cell.pop_back();
}

std::size_t GridService::queryRadius(const Vector3 &center, float radius, std::uint32_t typeMask,
                                     std::vector<Result> &out) const
{
    out.clear();
    forEachInRadius(center, radius, typeMask,
                    [&out](GridEntityType type, std::int32_t id, float distSq) { out.push_back({type, id, distSq}); });
    return out.size();
}

bool GridService::closest(const Vector3 &center, float radius, std::uint32_t typeMask, Result &out) const
{
    bool found = false;
    float best = radius * radius + 1.0f;
    forEachInRadius(center, radius, typeMask,
                    [&](GridEntityType type, std::int32_t id, float distSq)
                    {
                        if (distSq < best)
                        {
                            best = distSq;
                            out = {type, id, distSq};
                            found = true;
                        }
                    });
    return found;
}
