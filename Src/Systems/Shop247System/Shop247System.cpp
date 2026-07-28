#include "Systems/Shop247System/Shop247System.h"

#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Systems/MedkitSystem/MedkitSystem.h"
#include "Systems/ToolkitSystem/ToolkitSystem.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>
#include <string>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

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
} // namespace

const std::vector<Shop247System::Good> &Shop247System::goods()
{
    // Ассортимент — типы предметов из InventoryService: сам магазин своих вещей не
    // заводит, он лишь продаёт уже существующие (см. Docs/Inventory.md). Цена —
    // единственная величина, которую добавляет магазин.
    static const std::vector<Good> list = {
        {MedkitSystem::ITEM_MEDKIT, 250},
        {ToolkitSystem::ITEM_TOOLKIT, 400},
    };
    return list;
}

Shop247System::Shop247System(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_businessService(serviceRegister.getService<BusinessService>()),
      m_inventory(serviceRegister.getService<InventoryService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    m_businessService.registerType(BusinessService::Type::Shop247, "Магазин 24/7", shopCatalog(),
                                   [this](IPlayer &player, int businessId)
                                   {
                                       showShop(player, businessId);
                                   });
}

void Shop247System::showShop(IPlayer &player, int businessId)
{
    const int playerId = player.getID();

    std::string body = "Товар\tЦена\tУ вас\n";
    for (const Good &good : goods())
    {
        body += fmt::format("{}\t${}\t{}\n", m_inventory.itemName(good.itemType), good.price,
                            m_inventory.count(playerId, good.itemType));
    }
    body.pop_back();

    m_dialogService.show(
        player, makeDialog(DialogStyle_TABLIST_HEADERS, "Магазин 24/7", body, "Купить", "Закрыть"),
        [this, playerId, businessId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *buyer = m_core.getPlayers().get(playerId);
            if (!buyer || response != DialogResponse_Left)
            {
                return;
            }
            if (listItem < 0 || listItem >= static_cast<int>(goods().size()))
            {
                return; // индекс от клиента — проверяем по живому ассортименту
            }
            buy(*buyer, businessId, static_cast<std::size_t>(listItem));
        });
}

void Shop247System::buy(IPlayer &player, int businessId, std::size_t goodIndex)
{
    const int playerId = player.getID();
    const Good &good = goods()[goodIndex];

    // Стек полон — отказ ДО списания денег: иначе игрок платил бы за то, что не
    // влезет (add клампится к maxStack и вернул бы 0).
    if (m_inventory.count(playerId, good.itemType) >= m_inventory.maxStack(good.itemType))
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Больше {} у вас не поместится",
                                               m_inventory.itemName(good.itemType))));
        return;
    }
    // Баланс — серверный; take сам отказывает, если денег не стало.
    if (!m_moneyService.take(player, static_cast<unsigned long long>(good.price)))
    {
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Не хватает денег: нужно ${}", good.price)));
        return;
    }
    if (m_inventory.add(playerId, good.itemType, 1) <= 0)
    {
        // Гонка (стек заполнился между проверкой и выдачей) — деньги немедленно назад.
        m_moneyService.giveMoney(player, static_cast<unsigned long long>(good.price));
        return;
    }

    // Выручка — в копилку ЭТОГО бизнеса: владелец заберёт её через «Управление».
    // Бизнес мог быть снесён, пока висел диалог — тогда доход просто некуда класть.
    m_businessService.addIncome(businessId, good.price);

    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Куплено: {} за ${}. Теперь у вас {}",
                                           m_inventory.itemName(good.itemType), good.price,
                                           m_inventory.count(playerId, good.itemType))));
    showShop(player, businessId); // список заново — с обновлёнными остатками
}
