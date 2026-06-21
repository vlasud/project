#pragma once

#include "Services/PlayerPersonalSkinService/PlayerPersonalSkinService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник личного скина аккаунта: лайфцикл слота и персист в БД.
//
//  * коннект/дисконнект — сброс слота PlayerPersonalSkinService к «не сидирован»
//    (как PlayerSkinSystem чистит применённый скин), чтобы личный скин одного
//    игрока не утёк новому в том же слоте;
//  * персист (subscribeSave) — дописываем ЛИЧНЫЙ скин в `player.skin`
//    (belt-and-suspenders: основной путь — write-through при изменении и дефолт
//    при регистрации). Пишем значение из PlayerPersonalSkinService, НЕ применённый
//    (возможно органный) скин из PlayerSkinService. Идемпотентный UPDATE, поэтому
//    в save-канал: зовётся на конце сессии (до teardown) И автосейвом онлайн-игроков
//    (см. Docs/Autosave.md).
//
// Загрузку и применение личного скина на ВХОДЕ делает PlayerAuthSystem (у него
// уже логин-select): он сидирует сервис и применяет скин ДО старта членства,
// чтобы органный скин захватил правильный «гражданский» для возврата.
class PlayerPersonalSkinSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    PlayerPersonalSkinSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PlayerPersonalSkinService &m_personalSkinService;
    PlayerSessionService &m_sessionService;
};
