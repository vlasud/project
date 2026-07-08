#pragma once

#include "Macro.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/PlayerMoneyPersistService/PlayerMoneyPersistService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Персист наличных (`player_money`) аккаунта. Загрузка/сохранение — эта
// система, применение на логин-спавне (ресинхрон HUD) остаётся в
// PlayerAuthSystem (см. Docs/Persistence.md).
//
//  * коннект/дисконнект — сброс кэша слота, чтобы данные одного аккаунта не
//    утекли следующему игроку в том же playerId-слоте;
//  * старт сессии (subscribeStart) — async-select по account_id с
//    serial-guard (как InventorySystem/SpawnChoiceSystem). Наличные не
//    сбрасываются на спавне — колбэк пишет их СРАЗУ в PlayerMoneyService (как
//    InventorySystem::loadItems), затем взводит флаг загрузки;
//  * сохранение (subscribeSave) — снимок ТЕКУЩЕГО состояния
//    (PlayerMoneyService::getMoney) идемпотентным UPSERT'ом, гейтится
//    isMoneyLoaded (значение уже реально в сервисе — см. выше). Зовётся на
//    конце сессии И периодически автосейвом (см. Docs/Autosave.md).
class PlayerMoneyPersistSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    PlayerMoneyPersistSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    void loadMoney(IPlayer &player, const PlayerSessionService::Session &session);
    void persistMoney(IPlayer &player, const PlayerSessionService::Session &session);

    PlayerMoneyPersistService &m_persistService;
    PlayerMoneyService &m_moneyService;
    PlayerSessionService &m_sessionService;
};
