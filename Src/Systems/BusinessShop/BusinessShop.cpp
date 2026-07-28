#include "Systems/BusinessShop/BusinessShop.h"

#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/MoneyFormat/MoneyFormat.h"
#include <fmt/format.h>
#include <utility>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};
} // namespace

BusinessShop::BusinessShop(ICore &core, const ServiceRegister &serviceRegister, std::string title,
                           std::vector<Good> goods)
    : m_core(core), m_businessService(serviceRegister.getService<BusinessService>()),
      m_inventory(serviceRegister.getService<InventoryService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()), m_title(std::move(title)),
      m_goods(std::move(goods))
{
}

void BusinessShop::show(IPlayer &player, int businessId)
{
    const int playerId = player.getID();

    std::string body = "Товар\tЦена\tУ вас\n";
    for (const Good &good : m_goods)
    {
        body += fmt::format("{}\t{}\t{}\n", m_inventory.itemName(good.itemType), Money::text(good.price),
                            m_inventory.count(playerId, good.itemType));
    }
    body.pop_back();

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, m_title, body, "Купить", "Закрыть"),
                         [this, playerId, businessId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *buyer = m_core.getPlayers().get(playerId);
                             if (!buyer || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             if (listItem < 0 || listItem >= static_cast<int>(m_goods.size()))
                             {
                                 return; // индекс от клиента — проверяем по живому ассортименту
                             }
                             buy(*buyer, businessId, static_cast<std::size_t>(listItem));
                         });
}

void BusinessShop::buy(IPlayer &player, int businessId, std::size_t goodIndex)
{
    const int playerId = player.getID();
    const Good &good = m_goods[goodIndex];

    // Стек полон — отказ ДО списания денег: иначе игрок платил бы за то, что не
    // влезет (add клампится к maxStack и вернул бы 0).
    if (m_inventory.count(playerId, good.itemType) >= m_inventory.maxStack(good.itemType))
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Больше {} у вас не поместится", m_inventory.itemName(good.itemType))));
        return;
    }
    // Баланс — серверный; take сам отказывает, если денег не стало.
    if (!m_moneyService.take(player, static_cast<unsigned long long>(good.price)))
    {
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Не хватает денег: нужно {}", Money::text(good.price))));
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
                             u(fmt::format("Куплено: {} за {}. Теперь у вас {}", m_inventory.itemName(good.itemType),
                                           Money::text(good.price), m_inventory.count(playerId, good.itemType))));
    show(player, businessId); // список заново — с обновлёнными остатками
}
