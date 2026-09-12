#pragma once

#include "Macro.h"
#include "Services/Core/CameraService/CameraService.h"
#include "Services/Core/PlayerBubbleService/PlayerBubbleService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/ReturnPointService/ReturnPointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Точка возврата (`player_return_point`): где игрок был в конце прошлой сессии, и
// предложение вернуться туда на логин-спавне (см. Docs/ReturnPoint.md).
// Бизнес-фича персиста, вне Core.
//
//  * коннект/дисконнект и конец сессии (subscribeEnd) — сброс слота: точка и флаги
//    принадлежат РОВНО одной сессии и не должны утечь ни следующему игроку в том же
//    playerId-слоте, ни новой сессии на том же подключении (смена аккаунта);
//  * старт сессии (subscribeStart) — async-select по account_id с serial-guard
//    (как PlayerWeaponPersistSystem); результат кладётся в ReturnPointService;
//  * первый спавн сессии (onPlayerSpawn при активной сессии) — диалог «Вернуться /
//    Остаться». На спавнах до логина (класс-селекшн, спектейт авторизации) и на
//    респавнах после смерти диалога нет — гейт по isSpawned. Если загрузка из БД не
//    успела к спавну, диалог показывает её колбэк;
//  * «Вернуться» — телепорт через PlayerLocationService (позиция + интерьер + мир +
//    угол) и чат-бабл над головой. ТОЧКУ СПАВНА фича не трогает: её единственный
//    писатель — SpawnChoiceSystem (см. Docs/SetSpawn.md), возврат идёт телепортом
//    ПОСЛЕ спавна. Ответ проходит гейты серверного состояния: сессия (serial),
//    живой игрок, живость, PlayerState_OnFoot (серверный телепорт машину за собой
//    не тянет — как /tp в GpsSystem) и срок предложения (PROMPT_TTL от момента
//    ЛОГИН-СПАВНА, не от показа): колбэк диалога сам по себе не истекает, и без
//    срока модифицированный клиент придержал бы ответ до боя и получил мгновенный
//    эскейп-телепорт. Любой отказ — сообщение в чат, предложение считается
//    потраченным (promptDone не откручиваем);
//  * сохранение (subscribeSave) — снимок серверной позиции/интерьера/мира из
//    PlayerLocationService идемпотентным UPSERT. Два гейта: isSpawned (до
//    логин-спавна снимок затёр бы в БД реальную точку аккаунта) и
//    PlayerLocationService::hasPosition (сброшенный слот локации отдаёт нулевой
//    вектор — для мира ЛЕГАЛЬНУЮ точку, которую валидация пропустит). Зовётся на
//    конце сессии И периодическим автосейвом (см. Docs/Autosave.md).
class ReturnPointSystem : public BaseSystem, public PlayerConnectEventHandler, public PlayerSpawnEventHandler
{
  public:
    ReturnPointSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;
    void onPlayerSpawn(IPlayer &player) override;

  private:
    void loadPoint(IPlayer &player, const PlayerSessionService::Session &session);
    void persistPoint(IPlayer &player, const PlayerSessionService::Session &session);
    // Предложить возврат, если сошлось всё: логин-спавн пройден, загрузка пришла,
    // точка есть, диалог ещё не показывали. Зовётся из обеих точек гонки (спавн и
    // колбэк загрузки) — кто окажется вторым, тот и покажет диалог.
    void offerReturn(IPlayer &player, const PlayerSessionService::Session &session);
    // Спавн уже совпал с сохранённой точкой (позиция + интерьер + мир) — предлагать
    // возврат незачем.
    bool spawnedAtSamePlace(int playerId) const;

    void showDialog(IPlayer &player, const PlayerSessionService::Session &session);
    void returnToPoint(IPlayer &player);

    ReturnPointService &m_returnService;
    PlayerSessionService &m_sessionService;
    PlayerLocationService &m_locationService;
    PlayerStateService &m_stateService;
    PlayerHealthService &m_healthService;
    PlayerDialogService &m_dialogService;
    PlayerBubbleService &m_bubbleService;
    CameraService &m_cameraService;
};
