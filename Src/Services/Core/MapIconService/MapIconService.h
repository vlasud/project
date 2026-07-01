#pragma once

#include "Macro.h"
#include "Services/Core/StreamerService/StreamerService.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>

class MapIconSystem;

// Сервис иконок карты. Два вида:
//
//  * Глобальные — видны всем, стримятся по близости через StreamerService
//    (слоты 0..89 у каждого игрока), количество не ограничено клиентским
//    лимитом:
//        int id = m_mapIcons.addGlobal(56, {x, y, z});       // и потом
//        m_mapIcons.removeGlobal(id);
//
//  * Персональные — видны одному игроку (маркер цели, квестовая точка),
//    без стриминга, слоты 90..99 (до 10 на игрока), живут до удаления или
//    выхода игрока:
//        int h = m_mapIcons.setForPlayer(player, 41, pos, Colour::White(), MapIconStyle_Global);
//        m_mapIcons.updateForPlayer(player, h, newPos); // двигать маркер
//        m_mapIcons.removeForPlayer(player, h);
//
// Все параметры валидируются: тип иконки клампится к 0..63 (значения выше
// клиент не знает), стиль — к диапазону enum, NaN в координатах обнуляется.
class MapIconService final : public IService
{
    friend MapIconSystem;

  public:
    static constexpr int MAX_ICON_TYPE = 63; // радарные иконки SA: 0..63

    // --- глобальные (через стример) ---
    // Возвращает id иконки или -1.
    // streamDistance — радиус, в котором иконка стримится игроку (по умолчанию
    // максимум стримера). Меньше -> иконка видна только вблизи.
    int addGlobal(int iconType, const Vector3 &position, Colour colour = Colour::White(),
                  MapIconStyle style = MapIconStyle_Local,
                  float streamDistance = StreamerService::MAX_STREAM_DISTANCE);
    void removeGlobal(int iconId);

    // --- персональные (слоты 90..99 конкретного игрока) ---
    // Возвращает хэндл или -1, если свободных слотов нет.
    int setForPlayer(IPlayer &player, int iconType, const Vector3 &position, Colour colour = Colour::White(),
                     MapIconStyle style = MapIconStyle_Local);
    // Переустановить существующую иконку (двигать маркер, сменить тип/цвет).
    bool updateForPlayer(IPlayer &player, int handle, int iconType, const Vector3 &position,
                         Colour colour = Colour::White(), MapIconStyle style = MapIconStyle_Local);
    bool removeForPlayer(IPlayer &player, int handle);
    void clearForPlayer(IPlayer &player);
    int personalCount(int playerId) const;

  private:
    // Слоты 0..ICON_BUDGET-1 занимает StreamerService, клиентский лимит — 100.
    // Связка с бюджетом стримера закреплена static_assert в initialize().
    static constexpr int FIRST_MANUAL_SLOT = 90;
    static constexpr int MANUAL_SLOTS = 10;

    // Вызываются MapIconSystem.
    void initialize(StreamerService *streamer);
    void resetPlayer(int playerId);

    static int clampIconType(int iconType);
    static MapIconStyle clampStyle(MapIconStyle style);
    static Vector3 sanitizePosition(Vector3 position);

    StreamerService *m_streamer = nullptr;
    // m_used[playerId][i] — занят ли ручной слот FIRST_MANUAL_SLOT + i.
    std::array<std::array<bool, MANUAL_SLOTS>, MAX_PLAYERS> m_used{};
};
