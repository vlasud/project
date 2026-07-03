#pragma once

#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/HouseService/HouseService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Меню владельца дома (/home) — бизнес-фича (НЕ Core). Код: Src/Systems/HomeMenuSystem.
// Собственного сервиса нет — источник владения/контента дома — HouseService,
// парковка у дома — ParkedVehicleService, красный чекпоинт-указатель — общий
// VehicleWaypointService (target-режим «точка», см. его комментарий).
//
// Гейт входа: у игрока есть СВОЙ дом (HouseService::houseOf по серверному
// accountId сессии). Нет дома -> сообщение, меню не открывается. Корневое LIST
// («Мой дом») — 6 пунктов, ВСЕ видны ВСЕГДА (правило видимости, как CarMenuSystem):
// Информация / Отметить на карте / Улучшения (заглушка) / Передать владение /
// Продать (заглушка) / Выселиться. Ре-валидация houseOf на КАЖДОМ клике каждого
// пункта — диалог мог висеть, пока дом передавался/продавался из другого окна.
//
// ПЕРЕДАЧА и ВЫСЕЛЕНИЕ идут через общий гейт занятости (ни одна из припаркованных
// машин владельца не занята водителем) и общее действие «на парковку все машины»
// (ParkedVehicleService::unpark по каждой записи владельца — уничтожает экземпляр
// + DELETE строки, шеринг семье уходит вместе с записью). Смена/снятие владельца
// дома идёт через HouseService::setOwner — персист в БД (house_owner) и
// пересоздание иконки делает HouseSystem по подписке subscribeOwnerChanged (ЕДИНАЯ
// точка для занятия/передачи/выселения, см. комментарий HouseSystem).
//
// Навигация: «Назад» карточки/подтверждений -> корень /home. После действия 2
// (карта) — только сообщение, диалог не переоткрывается (как «Показать на
// карте» в /car); после 3/5 (улучшения/продажа — заглушки) — сообщение и
// возврат в корень. После 4/6 (передача/выселение) — дома у игрока больше нет,
// меню НЕ переоткрывается.
class HomeMenuSystem : public BaseSystem
{
  public:
    HomeMenuSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // --- вход /home ---
    void showRoot(IPlayer &player);

    // --- пункты корня ---
    void showInfo(IPlayer &player);           // 1: карточка дома (TABLIST_HEADERS)
    void showOnMap(IPlayer &player);          // 2: чекпоинт-указатель на вход дома
    void showUpgrades(IPlayer &player);       // 3: заглушка
    void showTransferInput(IPlayer &player);  // 4: INPUT id получателя
    void showSellStub(IPlayer &player);       // 5: заглушка
    void showEvictConfirm(IPlayer &player);   // 6: MSGBOX выселения

    // Подтверждение передачи получателю recipientId (INPUT уже дал число; здесь и
    // на согласии MSGBOX всё ре-валидируется заново — двойной клик/гонка).
    void showTransferConfirm(IPlayer &player, int recipientId);
    // expectedAccount — аккаунт получателя НА МОМЕНТ показа MSGBOX: слот recipientId
    // мог быть переиспользован другим игроком, пока подтверждение висело — дом не
    // должен уйти не тому, кого показали в окне.
    void executeTransfer(IPlayer &player, int recipientId,
                         PlayerSessionService::AccountId expectedAccount); // на согласии MSGBOX
    void executeEvict(IPlayer &player);                     // на согласии MSGBOX

    // Гейт занятости: true, если ХОТЯ БЫ ОДНА из припаркованных машин владельца
    // сейчас занята водителем (нельзя destroy под сидящим). O(записей владельца).
    bool anyParkedOccupied(PlayerSessionService::AccountId accountId) const;
    // Убрать ВСЕ припаркованные машины владельца на парковку (unpark по каждой
    // записи) — общее действие передачи/выселения. Вызывающий уже прошёл гейт
    // занятости. Перед КАЖДЫМ unpark снимает актуальный остаток бака (живой
    // экземпляр -> VehicleService::getFuel, иначе последний снимок parked->fuel) и
    // переносит его в PersonalVehicleService::setFuel владельца — как штатный
    // CarMenuSystem::unpark, иначе центральная парковка на следующем спавне возьмёт
    // устаревший (до-парковочный) бак записи владения. playerId владельца нужен для
    // адресации PersonalVehicleService (он вызывает это меню сам, т.е. онлайн).
    // O(записей владельца).
    void unparkAllOf(int playerId, PlayerSessionService::AccountId accountId);

    HouseService &m_houseService;
    ParkedVehicleService &m_parkedService;
    PersonalVehicleService &m_personalService;
    VehicleService &m_vehicleService;
    PlayerSessionService &m_sessionService;
    PlayerDialogService &m_dialogService;
    VehicleWaypointService &m_waypointService;
};
