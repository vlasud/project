#pragma once

#include "Services/BusinessService/BusinessService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/InventoryService/InventoryService.h"
#include "Services/ServiceRegister.h"
#include "player.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Витрина бизнеса — торговля предметами за наличные с выручкой в копилку точки.
// НЕ система и НЕ сервис: общий кусок ПОВЕДЕНИЯ, который берут себе типы бизнеса
// (24/7, АЗС и далее). Правила покупки у них совпадают до буквы, и вторая копия
// этого кода разъехалась бы на первой же правке — а это чужие деньги и предметы.
//
// Тип бизнеса держит витрину членом и отдаёт её show() в свой visitorMenu; сам
// ассортимент и заголовок задаются при создании — они у типов РАЗНЫЕ, общая
// только механика.
//
// Магазин своих вещей не заводит: он торгует уже существующими типами предметов
// из InventoryService (см. Docs/Inventory.md). Цена — единственное, что добавляет
// сама витрина.
class BusinessShop
{
  public:
    // Товар на полке: тип предмета из InventoryService + цена.
    struct Good
    {
        int itemType;
        std::int64_t price;
    };

    BusinessShop(ICore &core, const ServiceRegister &serviceRegister, std::string title, std::vector<Good> goods);

    // Показать витрину посетителю точки. Зовётся из visitorMenu типа.
    void show(IPlayer &player, int businessId);

  private:
    void buy(IPlayer &player, int businessId, std::size_t goodIndex);

    ICore &m_core;
    BusinessService &m_businessService;
    InventoryService &m_inventory;
    PlayerMoneyService &m_moneyService;
    PlayerDialogService &m_dialogService;

    std::string m_title;
    std::vector<Good> m_goods;
};
