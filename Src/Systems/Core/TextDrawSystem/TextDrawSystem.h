#pragma once

#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Systems/BaseSystem.h"
#include "Server/Components/TextDraws/textdraws.hpp"
#include "player.hpp"

// Единственный подписчик на события textdraw: передаёт сервису компонент пула,
// маршрутизирует клики/отмену выбора в TextDrawService и чистит per-player
// состояние при отключении игрока.
class TextDrawSystem : public BaseSystem, public TextDrawEventHandler, public PlayerConnectEventHandler
{
  public:
    TextDrawSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerClickTextDraw(IPlayer &player, ITextDraw &textDraw) override;
    void onPlayerClickPlayerTextDraw(IPlayer &player, IPlayerTextDraw &textDraw) override;
    bool onPlayerCancelTextDrawSelection(IPlayer &player) override;
    bool onPlayerCancelPlayerTextDrawSelection(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    TextDrawService &m_textDrawService;
};
