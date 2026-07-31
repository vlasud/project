#include "Systems/BusinessShop/BusinessShop.h"

#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/MoneyFormat/MoneyFormat.h"
#include <fmt/format.h>
#include <utility>
#include <vector>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Попап покупки: короткий, только название товара — подробности уже в чате.
constexpr Milliseconds PURCHASE_POPUP_TIME{2000};
// Звук кассы SA: подтверждение оплаты.
constexpr std::uint32_t PURCHASE_SOUND = 1054;

// Порог «мало» — доля потолка склада. Посетителю точные остатки не показываем:
// это внутренняя кухня владельца, а покупателю важно лишь «брать сейчас или искать
// другую точку». Владелец видит точные числа в «Инвентаризации».
constexpr int LOW_STOCK_PERCENT = 25;

// Как выглядит наличие товара для ПОСЕТИТЕЛЯ.
std::string availabilityText(int stock, int cap)
{
    if (stock <= 0)
    {
        return "Нет в наличии";
    }
    // cap <= 0 быть не должно (потолок задаёт ассортимент типа), но делить на него
    // вслепую нельзя — иначе битая регистрация уронит витрину.
    if (cap > 0 && stock * 100 <= cap * LOW_STOCK_PERCENT)
    {
        return "Мало";
    }
    return "Много";
}
} // namespace

BusinessShop::BusinessShop(ICore &core, const ServiceRegister &serviceRegister, std::string title,
                           BusinessService::Type type)
    : m_core(core), m_businessService(serviceRegister.getService<BusinessService>()),
      m_inventory(serviceRegister.getService<InventoryService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_noticeService(serviceRegister.getService<ScreenNoticeService>()),
      m_audioService(serviceRegister.getService<AudioService>()), m_title(std::move(title)), m_type(type)
{
}

void BusinessShop::show(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const std::vector<BusinessService::GoodDef> &goods = m_businessService.goods(m_type);
    const std::vector<BusinessService::ServiceDef> &services = m_businessService.services(m_type);

    std::string body = "Товар\tЦена\tНаличие\n";
    for (const BusinessService::GoodDef &good : goods)
    {
        body += fmt::format("{}\t{}\t{}\n", goodDisplayName(good, m_inventory.itemName(good.itemType)), Money::text(good.price),
                            availabilityText(m_businessService.stockOf(businessId, good.itemType), good.stockCap));
    }
    // Услуги — ХВОСТОМ списка (на этом держится разбор listItem). Склада у них нет,
    // поэтому в колонке наличия у всех одно и то же.
    for (const BusinessService::ServiceDef &service : services)
    {
        body += fmt::format("{}\t{}\tУслуга\n", service.name, Money::text(service.price));
    }
    body.pop_back();

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, m_title, body, "Выбрать", "Закрыть"),
                         [this, playerId, businessId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *buyer = m_core.getPlayers().get(playerId);
                             if (!buyer || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             // Индекс пришёл от клиента — проверяем по ЖИВЫМ спискам.
                             const std::size_t goodCount = m_businessService.goods(m_type).size();
                             const std::size_t serviceCount = m_businessService.services(m_type).size();
                             if (listItem < 0 || static_cast<std::size_t>(listItem) >= goodCount + serviceCount)
                             {
                                 return;
                             }
                             const auto index = static_cast<std::size_t>(listItem);
                             if (index < goodCount)
                             {
                                 showGood(*buyer, businessId, index);
                                 return;
                             }
                             showService(*buyer, businessId, index - goodCount);
                         });
}

void BusinessShop::showGood(IPlayer &player, int businessId, std::size_t goodIndex)
{
    const std::vector<BusinessService::GoodDef> &goods = m_businessService.goods(m_type);
    if (goodIndex >= goods.size())
    {
        return;
    }
    const int playerId = player.getID();
    const BusinessService::GoodDef &good = goods[goodIndex];
    const int stock = m_businessService.stockOf(businessId, good.itemType);

    // В LIST кликабельна КАЖДАЯ строка, поэтому справочных строк в теле нет: цена и
    // наличие вписаны в сами пункты, название товара — в заголовок.
    // ПОРЯДОК СТРОК = порядок case-веток обработчика.
    std::string body = fmt::format("Купить — {}\nИнформация\n\nВ продаже: {}", Money::text(good.price),
                                   availabilityText(stock, good.stockCap));
    // «У вас сейчас» осмысленно только у товара с предметом: у товара БЕЗ предмета
    // (телефон) считать в инвентаре нечего.
    if (!good.sell)
    {
        body += fmt::format("\nУ вас сейчас: {}", m_inventory.count(playerId, good.itemType));
    }

    m_dialogService.show(
        // Без u(): makeDialog сам прогоняет все поля через utf8Tocp1251, повторная
        // конвертация дала бы мусор.
        player, makeDialog(DialogStyle_LIST, goodDisplayName(good, m_inventory.itemName(good.itemType)), body, "Выбрать", "Назад"),
        [this, playerId, businessId, goodIndex](DialogResponse response, int listItem, StringView)
        {
            IPlayer *buyer = m_core.getPlayers().get(playerId);
            if (!buyer)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                show(*buyer, businessId); // «Назад» — к ассортименту
                return;
            }
            switch (listItem)
            {
            case 0:
                buy(*buyer, businessId, goodIndex);
                break;
            case 1:
                showInfo(*buyer, businessId, goodIndex);
                break;
            default:
                break; // справочные строки и пустая — без действия
            }
        });
}

void BusinessShop::showInfo(IPlayer &player, int businessId, std::size_t goodIndex)
{
    const std::vector<BusinessService::GoodDef> &goods = m_businessService.goods(m_type);
    if (goodIndex >= goods.size())
    {
        return;
    }
    const int playerId = player.getID();
    const BusinessService::GoodDef &good = goods[goodIndex];

    m_dialogService.show(player,
                         makeDialog(DialogStyle_MSGBOX, goodDisplayName(good, m_inventory.itemName(good.itemType)), good.description,
                                    "Назад", ""),
                         [this, playerId, businessId, goodIndex](DialogResponse, int, StringView)
                         {
                             IPlayer *visitor = m_core.getPlayers().get(playerId);
                             if (visitor)
                             {
                                 showGood(*visitor, businessId, goodIndex);
                             }
                         });
}

void BusinessShop::showService(IPlayer &player, int businessId, std::size_t serviceIndex)
{
    const std::vector<BusinessService::ServiceDef> &services = m_businessService.services(m_type);
    if (serviceIndex >= services.size())
    {
        return;
    }
    const int playerId = player.getID();
    const BusinessService::ServiceDef &service = services[serviceIndex];

    // ПОРЯДОК СТРОК = порядок case-веток обработчика (в LIST кликабельна каждая).
    const std::string body = fmt::format("Купить — {}\nИнформация\n\nУ вас: {}", Money::text(service.price),
                                         service.status ? service.status(playerId) : std::string("-"));

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, service.name, body, "Выбрать", "Назад"),
        [this, playerId, businessId, serviceIndex](DialogResponse response, int listItem, StringView)
        {
            IPlayer *buyer = m_core.getPlayers().get(playerId);
            if (!buyer)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                show(*buyer, businessId); // «Назад» — к ассортименту
                return;
            }
            // Список живой: пока висел диалог, услуг могло стать меньше.
            const std::vector<BusinessService::ServiceDef> &current = m_businessService.services(m_type);
            if (serviceIndex >= current.size())
            {
                return;
            }
            switch (listItem)
            {
            case 0:
                // Витрина НЕ списывает деньги и не трогает склад: продажу целиком
                // ведёт обработчик услуги (он же открывает свои диалоги).
                current[serviceIndex].sell(*buyer, businessId);
                break;
            case 1:
                showServiceInfo(*buyer, businessId, serviceIndex);
                break;
            default:
                break; // справочные строки и пустая — без действия
            }
        });
}

void BusinessShop::showServiceInfo(IPlayer &player, int businessId, std::size_t serviceIndex)
{
    const std::vector<BusinessService::ServiceDef> &services = m_businessService.services(m_type);
    if (serviceIndex >= services.size())
    {
        return;
    }
    const int playerId = player.getID();
    const BusinessService::ServiceDef &service = services[serviceIndex];

    m_dialogService.show(player, makeDialog(DialogStyle_MSGBOX, service.name, service.description, "Назад", ""),
                         [this, playerId, businessId, serviceIndex](DialogResponse, int, StringView)
                         {
                             IPlayer *visitor = m_core.getPlayers().get(playerId);
                             if (visitor)
                             {
                                 showService(*visitor, businessId, serviceIndex);
                             }
                         });
}

void BusinessShop::buy(IPlayer &player, int businessId, std::size_t goodIndex)
{
    const std::vector<BusinessService::GoodDef> &goods = m_businessService.goods(m_type);
    if (goodIndex >= goods.size())
    {
        return;
    }
    const int playerId = player.getID();
    const BusinessService::GoodDef &good = goods[goodIndex];

    // Склад пуст — отказ ДО денег и ДО стека: продавать нечего.
    if (m_businessService.stockOf(businessId, good.itemType) <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("{} закончились — товар не завезли",
                                                             goodDisplayName(good, m_inventory.itemName(good.itemType)))));
        showGood(player, businessId, goodIndex);
        return;
    }
    // Товар БЕЗ предмета продаёт себя сам: его продажа бывает отложенной (телефон
    // занимает номер в БД), поэтому деньги, склад и доход проводит обработчик.
    if (good.sell)
    {
        good.sell(player, businessId);
        return;
    }
    // Стек полон — отказ ДО списания денег: иначе игрок платил бы за то, что не
    // влезет (add клампится к maxStack и вернул бы 0).
    if (m_inventory.count(playerId, good.itemType) >= m_inventory.maxStack(good.itemType))
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Больше {} у вас не поместится", goodDisplayName(good, m_inventory.itemName(good.itemType)))));
        showGood(player, businessId, goodIndex); // карточка остаётся открытой и на отказе
        return;
    }
    // Баланс — серверный; take сам отказывает, если денег не стало.
    if (!m_moneyService.take(player, static_cast<unsigned long long>(good.price)))
    {
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Не хватает денег: нужно {}", Money::text(good.price))));
        showGood(player, businessId, goodIndex);
        return;
    }
    // Склад списываем ПОСЛЕ денег, но ДО выдачи предмета: если единицу увели в
    // гонке, деньги немедленно назад и предмет не выдаём.
    if (!m_businessService.consumeStock(businessId, good.itemType, 1))
    {
        m_moneyService.giveMoney(player, static_cast<unsigned long long>(good.price));
        player.sendClientMessage(ERROR_COLOUR, u("Последнюю единицу только что забрали"));
        showGood(player, businessId, goodIndex);
        return;
    }
    if (m_inventory.add(playerId, good.itemType, 1) <= 0)
    {
        // Гонка (стек заполнился между проверкой и выдачей) — деньги и товар назад.
        m_moneyService.giveMoney(player, static_cast<unsigned long long>(good.price));
        m_businessService.addStock(businessId, good.itemType, 1);
        return;
    }

    // Выручка — в копилку ЭТОГО бизнеса: владелец заберёт её через «Управление».
    // Бизнес мог быть снесён, пока висел диалог — тогда доход просто некуда класть.
    m_businessService.addIncome(businessId, good.price);

    // Попап (английский, как все экранные попапы проекта) + звук кассы: покупка
    // подтверждается без чтения чата, что важно при покупке пачкой.
    m_noticeService.show(player, good.popupName, PURCHASE_POPUP_TIME, INFO_COLOUR);
    m_audioService.playSound(player, PURCHASE_SOUND);

    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Куплено: {} за {}. Теперь у вас {}", goodDisplayName(good, m_inventory.itemName(good.itemType)),
                                           Money::text(good.price), m_inventory.count(playerId, good.itemType))));
    // ВОЗВРАЩАЕМ В КАРТОЧКУ, а не в список и не в закрытый диалог: предметы стековые,
    // и брать их пачкой — обычный сценарий, кнопка «Купить» остаётся под пальцем.
    showGood(player, businessId, goodIndex);
}
