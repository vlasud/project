#include "Systems/Shop247System/Shop247System.h"

#include "Systems/MedkitSystem/MedkitSystem.h"
#include "Systems/ToolkitSystem/ToolkitSystem.h"
#include <vector>

namespace
{
// Пул интерьеров 24/7 — замеры владельца (SA interior id + точка спавна внутри).
// Пятое поле — точка пикапа ВЫХОДА внутри интерьера; где оно не задано, привод
// ставит пикап смещением от точки спавна (см. BusinessService::CatalogEntry).
std::vector<BusinessService::CatalogEntry> shopCatalog()
{
    return {
        // «24/7 1» замерен полностью: точка входа и точка пикапа выхода. Угол в
        // замере 359.71 -> 0 (углы округляются до четверти оборота, см.
        // Geometry::snapToQuarterTurn): игрок появляется строго лицом на север.
        {"24/7 1", 17, Vector3(-25.9156f, -184.7823f, 1003.5469f), 0.0f,
         Vector3(-25.9765f, -188.2598f, 1003.5469f)},
        {"24/7 2", 10, Vector3(6.08f, -28.89f, 1003.54f), 0.0f},
        {"24/7 3", 18, Vector3(-30.98f, -89.68f, 1003.54f), 0.0f},
    };
}

// Ассортимент — типы предметов из InventoryService: сам магазин своих вещей не
// заводит, он лишь продаёт уже существующие (см. Docs/Inventory.md). Цена —
// единственная величина, которую добавляет магазин.
std::vector<BusinessShop::Good> shopGoods()
{
    return {
        {MedkitSystem::ITEM_MEDKIT, 250},
        {ToolkitSystem::ITEM_TOOLKIT, 400},
    };
}
} // namespace

Shop247System::Shop247System(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_businessService(serviceRegister.getService<BusinessService>()),
      m_shop(core, serviceRegister, "Магазин 24/7", shopGoods())
{
    m_businessService.registerType(BusinessService::Type::Shop247, "Магазин 24/7", shopCatalog(),
                                   [this](IPlayer &player, int businessId)
                                   {
                                       m_shop.show(player, businessId);
                                   });
}
