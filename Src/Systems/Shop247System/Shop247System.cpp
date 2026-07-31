#include "Systems/Shop247System/Shop247System.h"

#include "Systems/MedkitSystem/MedkitSystem.h"
#include <vector>

// Пул интерьеров 24/7 — замеры владельца: SA interior id, точка спавна внутри и
// УГОЛ появления. Угол не косметика: пикап выхода считается «за спиной» от точки
// спавна по нему же, поэтому неверный угол уводит и выход.
std::vector<BusinessService::CatalogEntry> Shop247System::interiors()
{
    return {
        // «24/7 1» замерен: угол 359.71 -> 0 (округление до четверти оборота,
        // Geometry::snapToQuarterTurn) — игрок появляется лицом на север, дверь
        // остаётся за спиной.
        // Пятое поле — ПРИЛАВОК: встав на него, посетитель получает витрину.
        // Шестое — ЗАМЕРЕННЫЙ пикап выхода. Он нужен именно здесь: дверь лежит в
        // 3.47 м за спиной, а правило считает выход в INSIDE_EXIT_DISTANCE = 2 м,
        // и расчётная точка не доходит до неё полтора метра.
        {"24/7 1", 17, Vector3(-25.9156f, -184.7823f, 1003.5469f), 0.0f,
         Vector3(-29.0621f, -185.1308f, 1003.5469f), Vector3(-25.9882f, -188.2590f, 1003.5469f)},
        {"24/7 2", 10, Vector3(6.08f, -28.89f, 1003.54f), 0.0f},
        {"24/7 3", 18, Vector3(-30.98f, -89.68f, 1003.54f), 0.0f},
    };
}

namespace
{
// Ассортимент — типы предметов из InventoryService: сам магазин своих вещей не
// заводит, он лишь продаёт уже существующие (см. Docs/Inventory.md). Цена —
// единственная величина, которую добавляет магазин.
// Третье поле — ПОТОЛОК СКЛАДА точки: сколько единиц влезает и до скольки владелец
// может дозаказать («Аптечка 13/100»).
std::vector<BusinessService::GoodDef> shopGoods()
{
    // Инструменты отсюда УБРАНЫ — они продаются на АЗС (см. Docs/GasStation.md):
    // ремонт машины логичнее покупать там, где машину и обслуживают.
    return {
        {MedkitSystem::ITEM_MEDKIT, 250, 100, MedkitSystem::shopDescription(), "medkit"},
    };
}
} // namespace

Shop247System::Shop247System(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_businessService(serviceRegister.getService<BusinessService>()),
      m_shop(core, serviceRegister, "Магазин 24/7", BusinessService::Type::Shop247)
{
    m_businessService.registerType(BusinessService::Type::Shop247, "Магазин 24/7", "24/7", Shop247System::interiors(), shopGoods(),
                                   [this](IPlayer &player, int businessId)
                                   {
                                       m_shop.show(player, businessId);
                                   });
}
