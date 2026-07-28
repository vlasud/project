#pragma once

#include "Macro.h"
#include "Services/AuctionService/AuctionService.h"
#include "Services/BusinessService/BusinessService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/TextLabelService/TextLabelService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <string>
#include <unordered_map>
#include <vector>

// Бизнесы — ОБЩИЙ привод BusinessService: то, что одинаково у всех типов.
// Геймплей конкретного типа живёт в своей системе (24/7 — Shop247System) и
// подключается регистрацией типа в сервисе.
//
// Здесь:
//  * дев-команда /business — создание (тип -> интерьер типа -> цена), список, снос;
//  * рантайм точки: пикап входа + иконка на карте + 3D-текст снаружи, пикап
//    выхода внутри;
//  * вход/выход (телепорт в уникальный vw бизнеса и обратно);
//  * ЕДИНЫЙ интерфейс «Бизнес» — меню внутри точки: информация, действие типа
//    (у 24/7 — покупка), управление для владельца (снять доход);
//  * АУКЦИОН ничейного бизнеса — вход внутрь заменяется торгами. Сами торги
//    (ставки, сроки, окна, деньги, /auc) ведёт общий AuctionService: здесь только
//    регистрация категории «Бизнесы» и открытие окна с пикапа.
//
// ДВА ПЕРСИСТА, как у домов:
//  * ОПИСАНИЕ — businesses.json рядом с сервером: пишем на любое изменение через
//    subscribeChanged, читаем на старте (ставки лежат в файле аукционов);
//  * ВЛАДЕНИЕ — БД (business_owner), write-through через ЕДИНУЮ точку
//    onOwnerChanged (подписка на BusinessService::subscribeOwnerChanged). UNIQUE
//    account_id в таблице — БД-бэкстоп к правилу «один бизнес на аккаунт».
// Рантайм-хэндлы пикапов и иконок в файл не идут — пересоздаются при загрузке.
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
    // Правка стартовой планки торгов уже созданного бизнеса (из списка).
    void showPriceEdit(IPlayer &player, int businessId);
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

    // Описание лота для общих торгов (колбэк категории «Бизнесы»). false — бизнеса
    // нет либо он уже чей-то, значит и торгов по нему нет.
    bool describeLot(int businessId, AuctionService::Lot &out) const;

    // --- персист ---
    void loadFromFileAsync();
    void saveToFileAsync();
    // Владение из БД: один selectQuery на старте, чейнится ПОСЛЕ разбора описаний
    // на ВСЕХ ветках (пусто/битый/успех) — иначе итоги аукционов не разблокируются.
    void loadOwnershipAsync();
    // ЕДИНАЯ точка персиста владения (подписка на subscribeOwnerChanged): пишет
    // business_owner одной транзакцией, при сбое откатывает память на oldKey.
    void onOwnerChanged(int businessId, const std::string &oldKey, const std::string &newKey);
    // Стереть строку владения снесённого бизнеса (removeBusiness мимо setOwner).
    void eraseOwnershipRow(int businessId);

    struct Runtime
    {
        int enterPickup = -1;
        int exitPickup = -1;
        int mapIcon = -1;
        int label = -1; // 3D-текст у входа (тип бизнеса + номер)
    };

    BusinessService &m_businessService;
    AuctionService &m_auctionService;
    PickupService &m_pickupService;
    MapIconService &m_mapIconService;
    PlayerDialogService &m_dialogService;
    PlayerLocationService &m_locationService;
    PlayerStateService &m_stateService;
    TextLabelService &m_labelService;
    PlayerMoneyService &m_moneyService;
    PlayerSessionService &m_sessionService;

    std::unordered_map<int, Runtime> m_runtime; // businessId -> хэндлы точек
};
