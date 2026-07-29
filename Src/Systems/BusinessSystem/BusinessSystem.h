#pragma once

#include "Macro.h"
#include "Services/AuctionService/AuctionService.h"
#include "Services/BusinessService/BusinessService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/CameraService/CameraService.h"
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
//  * ПОКУПКА ничейного бизнеса по ГОСЦЕНЕ — вход внутрь заменяется предложением
//    купить. Цену задаёт дев при создании (business.price), деньги списываются
//    сразу, бизнес сразу становится личным. В одни руки — один бизнес.
//    Аукцион госимуществом больше не торгует и остаётся под будущую продажу
//    имущества между игроками (категория «Бизнесы» зарегистрирована под неё).
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
    // Правка госцены уже созданного бизнеса (из списка).
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

    // Текст 3D-лейбла у входа: тип, номер и статус — «Владелец: ник» у занятого,
    // «Продаётся»/«Не продаётся» у ничейного (зависит от госцены). ownerName пуст у
    // ничейного либо когда ник неизвестен (владелец офлайн).
    std::string labelText(const BusinessService::Business &business, const std::string &ownerName) const;
    // Ник владельца по ключу аккаунта, если он СЕЙЧАС в сети ("" — офлайн/не найден).
    // Покупка бизнеса идёт только онлайн-игроку, поэтому там ник известен всегда.
    std::string onlineNameOf(const std::string &ownerKey) const;
    // Перерисовать лейбл (setText — живое обновление у тех, кто рядом).
    void refreshBusinessLabel(int businessId, const std::string &ownerName);

    // --- покупка из госсобственности ---
    // Предложение купить ничейный бизнес по ГОСЦЕНЕ (business.price). Показывается
    // на пикапе входа вместо прежнего окна торгов: госимущество продаётся сразу и по
    // фиксированной цене, аукцион остаётся для будущей продажи имущества игроками.
    void showPurchaseOffer(IPlayer &player, int businessId);
    // Купить бизнес. Вся проверка авторитетная и повторяется здесь: пока висел
    // диалог, бизнес могли занять/снести, цену — сменить, а деньги — потратить.
    void buyBusiness(IPlayer &player, int businessId);

    // Описание лота для общих торгов. ВСЕГДА false: госимущество больше не
    // разыгрывается (см. HouseSystem::describeLot — там же и зачем категория
    // остаётся зарегистрированной).
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
    CameraService &m_cameraService; // камера за спину после разворота на входе/выходе
    PlayerStateService &m_stateService;
    TextLabelService &m_labelService;
    PlayerMoneyService &m_moneyService;
    PlayerSessionService &m_sessionService;

    std::unordered_map<int, Runtime> m_runtime; // businessId -> хэндлы точек
};
