#pragma once

#include "../../Macro.h"
#include "../../Services/GridService/GridService.h"
#include "../../Services/PlayerLocationService/PlayerLocationService.h"
#include "../../Services/StreamerService/StreamerService.h"
#include "../BaseSystem.h"
#include "player.hpp"
#include <array>
#include <vector>

// Отладка сетки и стримера прямо в игре:
//  /gridtest    — построить тестовое поле вокруг игрока (конусы, пикапы, иконки)
//                 и включить оба режима уведомлений;
//  /gridclear   — убрать тестовое поле;
//  /gridcell    — вкл/выкл сообщения о переходе между ячейками сетки;
//  /streamdebug — вкл/выкл сообщения об изменении числа застримленного;
//  /near [радиус] — кто есть рядом по типам (запрос к сетке).
class GridDebugSystem : public BaseSystem, public PlayerUpdateEventHandler, public PlayerConnectEventHandler
{
  public:
    GridDebugSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    struct DebugState
    {
        bool cellNotify = false;
        bool streamNotify = false;
        int lastCx = -1;
        int lastCy = -1;
        std::size_t lastObjects = 0;
        std::size_t lastIcons = 0;
        int lastPickups = 0;
    };

    void buildTestField(IPlayer &player);
    void clearTestField(IPlayer &player);
    void reportNear(IPlayer &player, int radius);

    GridService &m_gridService;
    StreamerService &m_streamerService;
    PlayerLocationService &m_locationService;

    std::array<DebugState, MAX_PLAYERS> m_state;

    // Тестовое поле глобальное (контент один на сервер).
    std::vector<int> m_testObjects;
    std::vector<int> m_testPickups;
    std::vector<int> m_testIcons;
};
