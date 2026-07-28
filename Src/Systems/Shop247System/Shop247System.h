#pragma once

#include "Services/BusinessService/BusinessService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/InventoryService/InventoryService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <cstdint>
#include <vector>

// Магазин 24/7 — первый ТИП бизнеса поверх общей сущности. В конструкторе
// регистрирует себя в BusinessService (имя, пул интерьеров, меню посетителя) — как
// работы регистрируются в JobDismissService, а предметы в InventoryService.
//
// Общий привод (BusinessSystem) про 24/7 не знает ничего: он лишь зовёт
// зарегистрированное меню, когда игрок выбрал в интерфейсе «Бизнес» действие точки.
//
// Геймплей: продажа предметов (аптечки, инструменты) за наличные. Выручка идёт в
// копилку ЭТОГО бизнеса — владелец забирает её через «Управление бизнесом».
class Shop247System : public BaseSystem
{
  public:
    Shop247System(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Товар на полке: тип предмета из InventoryService + цена.
    struct Good
    {
        int itemType;
        std::int64_t price;
    };
    static const std::vector<Good> &goods();

    void showShop(IPlayer &player, int businessId);
    void buy(IPlayer &player, int businessId, std::size_t goodIndex);

    BusinessService &m_businessService;
    InventoryService &m_inventory;
    PlayerMoneyService &m_moneyService;
    PlayerDialogService &m_dialogService;
};
