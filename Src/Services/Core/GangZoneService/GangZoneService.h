#pragma once

#include "Services/IService.h"
#include "Server/Components/GangZones/gangzones.hpp"
#include "types.hpp"
#include <vector>

class GangZoneSystem;
struct ICore;

// Сервис ганг-зон с автоматической обрезкой перекрытий.
//
// Логическая зона — прямоугольник + цвет + приоритет (порядок в списке: чем
// раньше, тем выше приоритет). Перекрытия разрешаются всегда: зона младшего
// приоритета обрезается по всем старшим — её фактическая область разбивается
// на прямоугольные куски (отдельные IGangZone), наложений нет ни на радаре,
// ни в геометрии.
//
// Обрезка реальная, а не визуальная: zoneAtPoint()/isPointInZone() работают
// по обрезанной области, и любая точка мира принадлежит максимум ОДНОЙ зоне.
// Это гарантировано конструкцией: точка лежит в обрезанной области зоны тогда
// и только тогда, когда эта зона — старшая по приоритету из всех, чьи
// прямоугольники содержат точку (все пересечения у младших вырезаны).
// Фактические прямоугольники после обрезки доступны через getZonePieces().
//
// Зоны глобальные и показываются всем игрокам; показ новоприбывшим делает
// GangZoneSystem.
class GangZoneService final : public IService
{
    friend GangZoneSystem;

  public:
    struct ZoneInfo
    {
        int id = -1;
        GangZonePos rect{};
        Colour colour{};
        std::size_t pieceCount = 0; // на сколько кусков зону порезали перекрытия
    };

    bool isAvailable() const;

    // Монотонный счётчик мутаций набора зон (add/remove/clear/setRect/setColour/
    // приоритет). Редактор захватывает его при постановке async-load и сверяет в
    // колбэке перед clearZones: если набор изменился за окно чтения — устаревшую
    // загрузку отменяет, чтобы не затереть свежие правки другого админа.
    int getRevision() const;

    // Создать зону по двум противоположным углам (порядок углов любой,
    // координаты клампятся к миру). Новая зона получает низший приоритет.
    // Возвращает id зоны или -1.
    int addZone(Vector2 cornerA, Vector2 cornerB, Colour colour);
    void removeZone(int zoneId);
    void clearZones();

    bool setZoneRect(int zoneId, Vector2 cornerA, Vector2 cornerB);
    bool setZoneColour(int zoneId, Colour colour);
    // Сдвинуть приоритет на одну позицию: up — выше (обрезает больше соседей).
    bool moveZonePriority(int zoneId, bool up);

    bool getZone(int zoneId, ZoneInfo &out) const;
    std::vector<ZoneInfo> listZones() const; // в порядке приоритета (старшие первыми)

    // --- проверки по обрезанной геометрии ---
    // Зона, которой принадлежит точка (с учётом обрезки), или -1. Точка не
    // может принадлежать двум зонам. Границы полуоткрытые: [min, max).
    int zoneAtPoint(Vector2 point) const;
    int zoneAtPoint(const Vector3 &position) const; // по x/y
    // Точка внутри ОБРЕЗАННОЙ области зоны (вырезанные старшими участки — false).
    bool isPointInZone(int zoneId, Vector2 point) const;
    // Фактические прямоугольники зоны после обрезки.
    std::vector<GangZonePos> getZonePieces(int zoneId) const;

    // Подсветка зоны миганием для игрока (enable=false — выключить).
    void flashZone(IPlayer &player, int zoneId, Colour flashColour, bool enable);

  private:
    struct Zone
    {
        int id = -1;
        GangZonePos rect{};
        Colour colour{};
        std::vector<IGangZone *> pieces; // куски после обрезки старшими зонами
    };

    // Вызываются GangZoneSystem.
    void initialize(ICore *core, IGangZonesComponent *gangZones);
    void showAllForPlayer(IPlayer &player);

    // Пересобрать куски всех зон (геометрия изменилась) и показать всем.
    void rebuild();
    void releasePieces(Zone &zone);
    void showZoneToAll(const Zone &zone);
    Zone *findZone(int zoneId);
    const Zone *findZone(int zoneId) const;
    static GangZonePos normalizeRect(Vector2 a, Vector2 b);

    ICore *m_core = nullptr;
    IGangZonesComponent *m_gangZones = nullptr;
    std::vector<Zone> m_zones; // порядок = приоритет: первые обрезают последующих
    int m_nextId = 1;
    int m_revision = 0; // растёт на каждой мутации набора зон (см. getRevision)
};
