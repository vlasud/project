#pragma once

#include "Services/BusinessService/BusinessService.h"
#include "Systems/BaseSystem.h"
#include "Systems/BusinessShop/BusinessShop.h"
#include "player.hpp"

// Магазин 24/7 — первый ТИП бизнеса поверх общей сущности. В конструкторе
// регистрирует себя в BusinessService (имя, пул интерьеров, меню посетителя) — как
// работы регистрируются в JobDismissService, а предметы в InventoryService.
//
// Общий привод (BusinessSystem) про 24/7 не знает ничего: он лишь зовёт
// зарегистрированное меню, когда игрок выбрал в интерфейсе «Бизнес» действие точки.
//
// Геймплей: продажа предметов (аптечки, инструменты) за наличные. Сама торговля —
// общая витрина BusinessShop (её же берёт АЗС): здесь только имя типа, пул
// интерьеров и ассортимент. Выручка идёт в копилку ЭТОГО бизнеса — владелец
// забирает её через «Управление бизнесом».
class Shop247System : public BaseSystem
{
  public:
    // ЗАМЕРЕННЫЕ интерьеры магазина. Публичные, потому что АЗС сидит в ТЕХ ЖЕ
    // комнатах (отдельного интерьера у заправки в SA нет) и обязана брать те же
    // цифры: свои, «примерно такие же», уводят пикап выхода в стену, а прилавок —
    // мимо чекпоинта. Один замер — один источник.
    static std::vector<BusinessService::CatalogEntry> interiors();

    Shop247System(ICore &core, const ServiceRegister &serviceRegister);

  private:
    BusinessService &m_businessService;
    BusinessShop m_shop;
};
