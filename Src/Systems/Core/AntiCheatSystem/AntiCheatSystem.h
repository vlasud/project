#pragma once

#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Реакция на журнал нарушений: детекторы (здоровье, анимации, урон) пишут в
// AntiCheatService, эта система решает, что делать с игроком. Текущая политика —
// кик при достижении порога нарушений за скользящее окно. Кик — единственное
// действие, которое хакнутый клиент не может проигнорировать.
//
// Владеет жизненным циклом записей: чистит журнал при отключении игрока.
class AntiCheatSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    AntiCheatSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    void onViolation(int playerId, AntiCheatService::ViolationType type, const AntiCheatService::PlayerRecord &record);

    AntiCheatService &m_antiCheatService;
};
