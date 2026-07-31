#pragma once

#include "Macro.h"
#include "Services/AuctionService/AuctionService.h"
#include "Services/BusinessOrderService/BusinessOrderService.h"
#include "Services/BusinessService/BusinessService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/AudioService/AudioService.h"
#include "Services/Core/CameraService/CameraService.h"
#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/InventoryService/InventoryService.h"
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
    // Действия над КОНКРЕТНЫМ бизнесом (клик по строке списка): цена и колонка АЗС.
    void showBusinessActions(IPlayer &player, int businessId);
    void setFuelPointHere(IPlayer &player, int businessId); // колонка = там, где стоит дев
    void showFuelRadiusInput(IPlayer &player, int businessId);
    void clearFuelPoint(IPlayer &player, int businessId);
    void showDevRemove(IPlayer &player);
    // Дев-склад: точка -> выдать товар в нужном количестве либо очистить склад целиком.
    // Нужен для проверки витрины и заказов: без него набить склад можно только оплаченным
    // заказом, то есть полным рейсом развозчика.
    void showDevStockPick(IPlayer &player);
    void showDevStock(IPlayer &player, int businessId);
    void showDevStockAmount(IPlayer &player, int businessId, std::size_t goodIndex);
    void showDevStockClear(IPlayer &player, int businessId); // подтверждение очистки
    void clearDevStock(IPlayer &player, int businessId);

    // Завести чекпоинты-прилавки по каталогам ВСЕХ зарегистрированных типов (по
    // одному на интерьер, у которого замерен counter). Зовётся из initialize.
    void registerCounterCheckpoints();
    // Вошёл на прилавок: определяем бизнес по виртуальному миру игрока и открываем
    // витрину его типа.
    // Игрок встал на прилавок. Чекпоинт один на физическую точку — какой это бизнес
    // и какого он типа, решает виртуальный мир вошедшего.
    void onCounterEnter(IPlayer &player);

    // --- рантайм точки ---
    void spawnBusiness(const BusinessService::Business &business);
    void despawnBusiness(int businessId);
    void onEnterPickup(int businessId, IPlayer &player);
    void onExitPickup(int businessId, IPlayer &player);

    // --- управление своим бизнесом (/business) ---
    // Меню владельца. Бизнес определяется по аккаунту игрока (один на аккаунт).
    void showManageMenu(IPlayer &player);
    void showBusinessInfo(IPlayer &player, int businessId);    // 1: что это и как зарабатывать
    void showIncomeHistory(IPlayer &player, int businessId);   // 2: выручка по дням за неделю
    void showSellInput(IPlayer &player, int businessId);       // 5: кому продать
    void showSellPriceInput(IPlayer &player, int businessId, int targetId,
                            std::uint32_t targetSerial);       // 5: за сколько
    void showTransferInput(IPlayer &player, int businessId);   // 6: кому передать бесплатно
    void showAbandonConfirm(IPlayer &player, int businessId);  // 7: вернуть государству
    // Предложение покупателю/получателю. price == 0 — безвозмездная передача.
    void offerToTarget(IPlayer &seller, int businessId, int targetId, std::uint32_t targetSerial,
                       std::int64_t price);
    // Согласие принято: ПОЛНАЯ ре-валидация обеих сторон и сама сделка.
    void completeHandover(IPlayer &target, int businessId, PlayerSessionService::AccountId sellerAccount,
                          std::int64_t price);
    // Общая проверка «кому можно отдать точку»: онлайн, не сам себе, рядом, без
    // своего бизнеса. reason — что сказать инициатору при отказе.
    bool handoverTargetValid(IPlayer &owner, int targetId, std::uint32_t targetSerial, std::string &reason) const;

    // Дневная выручка в БД: строка на (бизнес, дата), правится ОТНОСИТЕЛЬНО.
    void persistIncomeDay(int businessId, std::int64_t income);

    // --- склад ---
    void showInventory(IPlayer &player, int businessId);   // остатки «13/100»
    void showOrderMenu(IPlayer &player, int businessId);   // выбрать к заказу / сделать заказ
    void showOrderPicker(IPlayer &player, int businessId); // тот же список + колонка заказа
    void showOrderAmountInput(IPlayer &player, int businessId, std::size_t goodIndex); // сколько заказать
    void showOrderConfirm(IPlayer &player, int businessId);                            // что и почём
    void showBonusInput(IPlayer &player, int businessId, std::int64_t cost);           // премия развозчикам
    void placeOrder(IPlayer &player, int businessId, std::int64_t bonus);              // оплата и заказ
    // Стоимость закупки по черновику игрока, склампленная фактическим местом на
    // складе. plan — что реально закажем (товар -> количество).
    std::int64_t planOrder(int playerId, int businessId, std::vector<std::pair<int, int>> &plan) const;

    // --- персист заказов ---
    void persistOrder(int orderId);
    void loadOrdersAsync();
    // Снять все заказы точки и вернуть владельцу деньги (снос точки девом).
    void refundOrdersOf(int businessId);
    // Отмена заказа владельцем — только пока его никто не повёз.
    void cancelOrder(IPlayer &player, int businessId);
    // Что дописать в строку отмены: есть ли у точки заказ и в каком он состоянии.
    // Владелец должен видеть это ДО клика — иначе «отменить заказ» приходится жать
    // наугад, чтобы узнать, был ли он вообще сделан.
    std::string orderRowStatus(int businessId) const;
    // Во сколько обошлась закупка заказа (без премии) по ценам его точки.
    std::int64_t orderCost(const BusinessOrderService::Order &order) const;
    // Черновик заказа игрока: тип предмета -> сколько заказать. Живёт до отправки
    // заказа либо до конца сессии — в БД ему делать нечего.
    std::unordered_map<int, int> &orderDraft(int playerId);
    void clearOrderDraft(int playerId);
    // Остаток склада в БД: строка на (бизнес, товар), АБСОЛЮТНАЯ запись под ключом
    // упорядочивания — у величины есть потолок, и относительная дельта при потере
    // записи увела бы её за него.
    void persistStock(int businessId, int itemType, int quantity);
    void loadStockAsync();

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
    CameraService &m_cameraService;         // камера за спину после разворота на входе/выходе
    CheckpointService &m_checkpointService; // чекпоинты-прилавки внутри интерьеров
    ScreenNoticeService &m_noticeService;   // попап с названием точки при входе
    AudioService &m_audioService;           // звук входа
    InventoryService &m_inventoryService;   // имена товаров в инвентаризации и заказе
    BusinessOrderService &m_orderService;   // заказы товара для развозчиков
    PlayerStateService &m_stateService;
    TextLabelService &m_labelService;
    PlayerMoneyService &m_moneyService;
    PlayerSessionService &m_sessionService;

    std::unordered_map<int, Runtime> m_runtime;
    // playerId -> (тип предмета -> заказанное количество). Черновик заказа.
    std::unordered_map<int, std::unordered_map<int, int>> m_orderDrafts; // businessId -> хэндлы точек
};
