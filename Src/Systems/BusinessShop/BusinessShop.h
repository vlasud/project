#pragma once

#include "Services/BusinessService/BusinessService.h"
#include "Services/Core/AudioService/AudioService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/InventoryService/InventoryService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/ServiceRegister.h"
#include "player.hpp"
#include <cstddef>
#include <string>

// Витрина бизнеса — торговля предметами за наличные с выручкой в копилку точки.
// НЕ система и НЕ сервис: общий кусок ПОВЕДЕНИЯ, который берут себе типы бизнеса
// (24/7, АЗС и далее). Правила покупки у них совпадают до буквы, и вторая копия
// этого кода разъехалась бы на первой же правке — а это чужие деньги и предметы.
//
// Ассортимент витрина НЕ хранит: он лежит в реестре типа (BusinessService::goods),
// потому что нужен ещё и меню владельца — инвентаризации и заказу. Две копии списка
// разъехались бы по ценам и потолкам склада.
//
// Товар продаётся СО СКЛАДА точки: каждая продажа списывает одну единицу, пустой
// склад продавать отказывается. Пополняет склад владелец через «Заказать товар».
//
// УСЛУГИ точки (BusinessService::ServiceDef) идут в списке ПОСЛЕ товаров. Витрина о
// них знает ровно одно: как показать карточку и кого позвать по «Купить». Деньги и
// доход проводит сам обработчик услуги — продажа бывает отложенной (проверка в БД) и
// может не состояться.
class BusinessShop
{
  public:
    BusinessShop(ICore &core, const ServiceRegister &serviceRegister, std::string title, BusinessService::Type type);

    // Показать витрину посетителю точки. Зовётся из visitorMenu типа.
    void show(IPlayer &player, int businessId);

  private:
    // Карточка товара: «Купить» / «Информация». Отдельный шаг, чтобы клик по строке
    // ассортимента не списывал деньги сразу.
    void showGood(IPlayer &player, int businessId, std::size_t goodIndex);
    // Справка по товару: механика и подводные камни, назад — в карточку.
    void showInfo(IPlayer &player, int businessId, std::size_t goodIndex);
    // Покупка. По успеху ВОЗВРАЩАЕТ в карточку товара, а не закрывает диалог:
    // предметы стековые, и брать их пачкой — обычный сценарий.
    void buy(IPlayer &player, int businessId, std::size_t goodIndex);

    // Карточка услуги и справка по ней — те же два шага, что у товара. Покупку
    // ведёт обработчик услуги: витрина только зовёт его.
    void showService(IPlayer &player, int businessId, std::size_t serviceIndex);
    void showServiceInfo(IPlayer &player, int businessId, std::size_t serviceIndex);

    ICore &m_core;
    BusinessService &m_businessService;
    InventoryService &m_inventory;
    PlayerMoneyService &m_moneyService;
    PlayerDialogService &m_dialogService;
    ScreenNoticeService &m_noticeService;   // попап с названием купленного
    AudioService &m_audioService;           // звук покупки

    std::string m_title;
    BusinessService::Type m_type; // чей ассортимент показываем
};
