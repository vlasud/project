#pragma once

#include "../../Macro.h"
#include "../../Services/AntiCheatService/AntiCheatService.h"
#include "../../Services/GridService/GridService.h"
#include "../../Services/PlayerLocationService/PlayerLocationService.h"
#include "../../Services/PlayerVelocityService/PlayerVelocityService.h"
#include "../BaseSystem.h"
#include "player.hpp"
#include <array>

// Отладка сервисов позиции и скорости прямо в игре:
//  /pos               — принятая позиция vs сырая клиентская, интерьер, VW, ячейка;
//  /tp [x] [y] [z]    — серверный телепорт через сервис (проверка грейса прибытия);
//  /tpup [метры]      — телепорт вверх: свободное падение для теста velocity;
//  /vw [id], /int [id] — смена виртуального мира / интерьера;
//  /hackpos [метры]   — СИМУЛЯЦИЯ телепорт-хака: сырой setPosition мимо сервиса,
//                       валидатор должен откатить и записать нарушение;
//  /vel               — текущая скорость: серверная (из позиций) vs клиентская;
//  /veldebug          — вкл/выкл поток скорости в чат (каждые 500 мс);
//  /violations        — журнал нарушений по себе.
class LocationDebugSystem : public BaseSystem, public PlayerUpdateEventHandler, public PlayerConnectEventHandler
{
  public:
    LocationDebugSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    struct DebugState
    {
        bool velNotify = false;
        TimePoint nextVelAt;
    };

    void showPos(IPlayer &player);
    void showVel(IPlayer &player);
    void showViolations(IPlayer &player);

    PlayerLocationService &m_locationService;
    PlayerVelocityService &m_velocityService;
    AntiCheatService &m_antiCheatService;

    std::array<DebugState, MAX_PLAYERS> m_state;
};
