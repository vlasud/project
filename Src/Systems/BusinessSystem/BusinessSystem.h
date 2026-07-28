#pragma once

#include "Macro.h"
#include "Services/BusinessService/BusinessService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <string>
#include <unordered_map>

// Бизнесы — ОБЩИЙ привод BusinessService: то, что одинаково у всех типов.
// Геймплей конкретного типа живёт в своей системе (24/7 — Shop247System) и
// подключается регистрацией типа в сервисе.
//
// Здесь:
//  * дев-команда /business — создание (тип -> интерьер типа -> цена), список, снос;
//  * рантайм точки: пикап входа + иконка на карте снаружи, пикап выхода внутри;
//  * вход/выход (телепорт в уникальный vw бизнеса и обратно);
//  * ЕДИНЫЙ интерфейс «Бизнес» — меню внутри точки: информация, действие типа
//    (у 24/7 — покупка), управление для владельца (снять доход).
//
// Персист — businesses.json рядом с сервером (как houses.json): пишем на любое
// изменение через subscribeChanged, читаем на старте. Рантайм-хэндлы пикапов и
// иконок в файл не идут — пересоздаются при загрузке.
class BusinessSystem : public BaseSystem
{
  public:
    BusinessSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    // --- дев-меню ---
    void showDevMenu(IPlayer &player);
    void showTypeChoice(IPlayer &player);
    void showInteriorChoice(IPlayer &player, BusinessService::Type type);
    void showPriceInput(IPlayer &player, BusinessService::Type type, int interiorIndex);
    void createBusiness(IPlayer &player, BusinessService::Type type, int interiorIndex, std::int64_t price);
    void showDevList(IPlayer &player);
    void showDevRemove(IPlayer &player);

    // --- рантайм точки ---
    void spawnBusiness(const BusinessService::Business &business);
    void despawnBusiness(int businessId);
    void onEnterPickup(int businessId, IPlayer &player);
    void onExitPickup(int businessId, IPlayer &player);

    // --- интерфейс «Бизнес» ---
    void showBusinessMenu(IPlayer &player, int businessId);
    void showOwnerMenu(IPlayer &player, int businessId);
    void withdrawIncome(IPlayer &player, int businessId);
    // Ключ владельца текущей сессии игрока ("" — не залогинен).
    std::string ownerKeyOf(int playerId) const;

    // --- персист ---
    void loadFromFileAsync();
    void saveToFileAsync();

    struct Runtime
    {
        int enterPickup = -1;
        int exitPickup = -1;
        int mapIcon = -1;
    };

    BusinessService &m_businessService;
    PickupService &m_pickupService;
    MapIconService &m_mapIconService;
    PlayerDialogService &m_dialogService;
    PlayerLocationService &m_locationService;
    PlayerStateService &m_stateService;
    PlayerMoneyService &m_moneyService;
    PlayerSessionService &m_sessionService;

    std::unordered_map<int, Runtime> m_runtime; // businessId -> хэндлы точек
};
