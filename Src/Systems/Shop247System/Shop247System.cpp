#include "Systems/Shop247System/Shop247System.h"

#include "Systems/MedkitSystem/MedkitSystem.h"
#include "Systems/ToolkitSystem/ToolkitSystem.h"
#include <vector>

namespace
{
// Пул интерьеров 24/7 — замеры владельца (SA interior id + точка спавна внутри).
// Угол 0: игрок появляется лицом от прилавка, поворот дев подстроит при надобности.
std::vector<BusinessService::CatalogEntry> shopCatalog()
{
    return {
        {"24/7 1", 17, Vector3(-25.72f, -187.82f, 1003.54f), 0.0f},
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
