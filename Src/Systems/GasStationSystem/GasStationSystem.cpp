#include "Systems/GasStationSystem/GasStationSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Systems/MedkitSystem/MedkitSystem.h"
#include "Systems/Shop247System/Shop247System.h"
#include "Systems/ToolkitSystem/ToolkitSystem.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/MoneyFormat/MoneyFormat.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fmt/format.h>
#include <string>
#include <vector>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Табличка у колонки: висит невысоко над землёй. Дистанцию отрисовки НЕ задаём
// константой — она равна радиусу самой колонки, чтобы «вижу табличку» и «могу
// заправиться» совпадали.
constexpr float PUMP_LABEL_HEIGHT = 0.9f;
const Colour PUMP_LABEL_COLOUR{255, 214, 10};

// Штатная иконка заправки на радаре SA.
constexpr int GAS_STATION_MAP_ICON = 47;

// Цена литра. Бак — 100 л, расход 0.1 л/сек, то есть полный бак ≈ 17 минут езды:
// заправка «под завязку» с сухого стоит 300$ за эти 17 минут. Величина
// балансная — правится одной строкой (см. Docs/GasStation.md).
constexpr std::int64_t FUEL_PRICE_PER_LITRE = 3;

// Ниже этого остатка доливать нечего — не гоняем диалог ради капли.
constexpr float MIN_REFUEL_LITRES = 1.0f;

// Пул интерьеров АЗС. Магазин при заправке в SA — тот же 24/7, отдельного интерьера
// у него нет, поэтому берём ЗАМЕРЫ МАГАЗИНА как есть и меняем только подпись в
// дев-меню. Свои «примерно такие же» координаты тут уже стояли и стоили двух багов:
// пикап выхода считается «за спиной» от точки спавна и уезжал в стену, а прилавок
// не совпадал с чекпоинтом комнаты.
std::vector<BusinessService::CatalogEntry> stationCatalog()
{
    std::vector<BusinessService::CatalogEntry> catalog = Shop247System::interiors();
    for (std::size_t index = 0; index < catalog.size(); ++index)
    {
        catalog[index].name = fmt::format("АЗС {}", index + 1);
    }
    return catalog;
}

// Ассортимент лавки при заправке. ИНСТРУМЕНТЫ — только здесь: в 24/7 их больше нет,
// ремонт машины покупают там, где машину и обслуживают (см. Docs/GasStation.md).
// Третье поле — потолок склада точки (см. Shop247System).
std::vector<BusinessService::GoodDef> stationGoods()
{
    return {
        {MedkitSystem::ITEM_MEDKIT, 250, 100, MedkitSystem::shopDescription(), "medkit"},
        {ToolkitSystem::ITEM_TOOLKIT, 400, 100, ToolkitSystem::shopDescription(), "toolkit"},
    };
}

// Стоимость долива с округлением ВВЕРХ: доливать по копейке бесплатно нельзя.
std::int64_t priceOf(float litres)
{
    return static_cast<std::int64_t>(std::ceil(litres)) * FUEL_PRICE_PER_LITRE;
}
} // namespace

GasStationSystem::GasStationSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_businessService(serviceRegister.getService<BusinessService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_labelService(serviceRegister.getService<TextLabelService>()),
      m_shop(core, serviceRegister, "Лавка при АЗС", BusinessService::Type::GasStation)
{
    m_businessService.registerType(BusinessService::Type::GasStation, "АЗС", "Gas Station", stationCatalog(),
                                   stationGoods(),
                                   [this](IPlayer &player, int businessId)
                                   {
                                       m_shop.show(player, businessId);
                                   });

    // Своя иконка радара: у заправки в SA она есть штатная, общая бизнес-иконка тут
    // не к месту.
    m_businessService.setTypeMapIcon(BusinessService::Type::GasStation, GAS_STATION_MAP_ICON);

    // ТОПЛИВО — товар БЕЗ ПРЕДМЕТА: в инвентарь не ложится, но на станции кончается
    // и владелец дозаказывает его через «Заказать товар», как аптечки. В витрине
    // покупать его нельзя — бак наливают у колонки, поэтому обработчик лишь
    // подсказывает, куда идти.
    BusinessService::GoodDef fuel;
    fuel.itemType = STOCK_FUEL; // ключ склада: предмета за этим числом нет
    fuel.name = "Топливо (литр)";
    fuel.price = FUEL_PRICE_PER_LITRE;
    fuel.stockCap = FUEL_STOCK_CAP;
    fuel.description = "Запас бензина в резервуаре станции.\n\n"
                       "Литры продаются НЕ здесь: подъезжайте к колонке и вводите /buyfuel.\n"
                       "Сколько осталось в резервуаре, написано на табличке у колонки.\n\n"
                       "Владельцу: резервуар пополняется через «Заказать товар», как и всё остальное.";
    fuel.popupName = "Fuel";
    fuel.sell = [this](IPlayer &player, int /*businessId*/)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Бензин наливают у колонки: подъезжайте и введите /buyfuel"));
    };
    m_businessService.addGood(BusinessService::Type::GasStation, std::move(fuel));

    // Таблички зависят от контента (колонку задали/убрали, бизнес снесли) и от
    // остатка топлива — подписываемся на оба канала. Плюс на ЗАГРУЗКУ: businesses.json
    // читается АСИНХРОННО (файл на воркере, разбор в колбэке главного потока), то есть
    // заведомо позже любого initialize(). Без этой подписки таблички не появлялись бы
    // после рестарта вовсе — только после первой правки контента.
    m_businessService.subscribeLoaded([this]() { rebuildPumpLabels(); });
    m_businessService.subscribeChanged([this]() { rebuildPumpLabels(); });
    m_businessService.subscribeStockChanged(
        [this](int businessId, int itemType, int /*quantity*/)
        {
            if (itemType == STOCK_FUEL)
            {
                refreshPumpLabel(businessId);
            }
        });

    serviceRegister.getService<PlayerCommandService>().add(
        "buyfuel", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            showFuelMenu(player);
        },
        {}, "заправить машину у колонки АЗС", PlayerCommandService::HelpCategory::Economy);
}

void GasStationSystem::initialize(IComponentList * /*components*/)
{
    // На старте тут ещё пусто: businesses.json грузится асинхронно, и таблички
    // ставит subscribeLoaded. Проход оставлен как страховка на случай, если точки
    // окажутся готовы раньше, — rebuildPumpLabels идемпотентен, повтор безвреден.
    rebuildPumpLabels();
}

// ------------------------------------------------------------------ таблички

std::string GasStationSystem::pumpText(int businessId) const
{
    const int litres = m_businessService.stockOf(businessId, STOCK_FUEL);
    return fmt::format("{} литров.\nВведите /buyfuel чтобы заправиться", litres);
}

void GasStationSystem::rebuildPumpLabels()
{
    // Снимаем таблички у тех, у кого колонки больше нет (убрали точку, снесли бизнес).
    for (auto it = m_pumpLabels.begin(); it != m_pumpLabels.end();)
    {
        const BusinessService::Business *business = m_businessService.getBusiness(it->first);
        if (!business || business->type != BusinessService::Type::GasStation || business->fuelRadius <= 0.0f)
        {
            m_labelService.remove(it->second);
            it = m_pumpLabels.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto &[id, business] : m_businessService.businesses())
    {
        if (business.type != BusinessService::Type::GasStation || business.fuelRadius <= 0.0f)
        {
            continue;
        }
        const Vector3 at(business.fuelPoint.x, business.fuelPoint.y, business.fuelPoint.z + PUMP_LABEL_HEIGHT);
        const auto existing = m_pumpLabels.find(id);
        if (existing != m_pumpLabels.end())
        {
            // Лейбл нельзя подвинуть — при смене места пересоздаём.
            m_labelService.remove(existing->second);
            m_pumpLabels.erase(existing);
        }
        // u(): текст лейбла уходит клиенту в cp1251, как и весь русский в проекте.
        // Дистанция отрисовки = радиусу колонки: табличку видно ровно там, где она
        // и работает, а не «ещё за квартал».
        const int labelId = m_labelService.add(u(pumpText(id)), at, PUMP_LABEL_COLOUR, business.fuelRadius);
        if (labelId >= 0)
        {
            m_pumpLabels[id] = labelId;
        }
    }
}

void GasStationSystem::refreshPumpLabel(int businessId)
{
    const auto it = m_pumpLabels.find(businessId);
    if (it != m_pumpLabels.end())
    {
        m_labelService.setText(it->second, u(pumpText(businessId)));
    }
}

int GasStationSystem::stationAtPump(const Vector3 &position) const
{
    // Линейно по бизнесам (их десятки) на холодном пути — команда игрока, не тик.
    int nearest = -1;
    float best = 0.0f;
    for (const auto &[id, business] : m_businessService.businesses())
    {
        // Колонка не задана — станция не обслуживает вовсе: заправка привязана к
        // ней, а не ко входу.
        if (business.type != BusinessService::Type::GasStation || business.fuelRadius <= 0.0f)
        {
            continue;
        }
        const float dx = business.fuelPoint.x - position.x;
        const float dy = business.fuelPoint.y - position.y;
        const float dz = business.fuelPoint.z - position.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        const float limit = business.fuelRadius * business.fuelRadius;
        if (distance <= limit && (nearest < 0 || distance < best))
        {
            best = distance;
            nearest = id;
        }
    }
    return nearest;
}

void GasStationSystem::showFuelMenu(IPlayer &player)
{
    const int playerId = player.getID();
    // За рулём — серверный стейт: пассажиру и пешеходу заправлять нечего.
    if (m_stateService.getState(playerId) != PlayerState_Driver)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Заправлять можно только сидя за рулём"));
        return;
    }
    IVehicle *vehicle = m_vehicleService.getVehicle(playerId);
    if (!vehicle)
    {
        return; // стейт водителя без машины — рассинхрон, молча выходим
    }
    // Позиция ПРИНЯТАЯ сервером: по клиентской можно было бы «дотянуться» до АЗС
    // с другого конца города.
    const int businessId = stationAtPump(m_locationService.getPosition(playerId));
    if (businessId < 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Рядом нет заправочной колонки"));
        return;
    }

    const int vehicleId = vehicle->getID();
    const float fuel = m_vehicleService.getFuel(vehicleId);
    const float free = std::max(0.0f, VehicleService::FUEL_CAPACITY - fuel);
    if (free < MIN_REFUEL_LITRES)
    {
        player.sendClientMessage(INFO_COLOUR, u("Бак уже полный"));
        return;
    }

    std::string body = fmt::format("В баке: {:.0f} из {:.0f} л\n", fuel, VehicleService::FUEL_CAPACITY);
    body += fmt::format("Цена: {} за литр\n\n", Money::text(FUEL_PRICE_PER_LITRE));
    body += fmt::format("Полный бак — {:.0f} л ({})\n", free, Money::text(priceOf(free)));
    body += "Ввести литры";

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "АЗС — заправка", body, "Выбрать", "Закрыть"),
                         [this, playerId, businessId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *driver = m_core.getPlayers().get(playerId);
                             if (!driver || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             if (listItem == 0)
                             {
                                 // Свободное место считаем заново: пока висел диалог,
                                 // топливо утекало дренажем.
                                 refuel(*driver, businessId, VehicleService::FUEL_CAPACITY);
                             }
                             else if (listItem == 1)
                             {
                                 showLitresInput(*driver, businessId);
                             }
                         });
}

void GasStationSystem::showLitresInput(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const std::string body =
        fmt::format("Сколько литров залить?\nЦена — {} за литр, в бак влезает {:.0f} л.",
                    Money::text(FUEL_PRICE_PER_LITRE), VehicleService::FUEL_CAPACITY);

    m_dialogService.show(player, makeDialog(DialogStyle_INPUT, "АЗС — заправка", body, "Залить", "Назад"),
                         [this, playerId, businessId](DialogResponse response, int, StringView text)
                         {
                             IPlayer *driver = m_core.getPlayers().get(playerId);
                             if (!driver)
                             {
                                 return;
                             }
                             if (response != DialogResponse_Left)
                             {
                                 showFuelMenu(*driver);
                                 return;
                             }
                             const std::string entered(text.data(), text.size());
                             char *end = nullptr;
                             const long long litres = std::strtoll(entered.c_str(), &end, 10);
                             if (entered.empty() || end == entered.c_str() || *end != '\0' || litres <= 0 ||
                                 litres > static_cast<long long>(VehicleService::FUEL_CAPACITY))
                             {
                                 driver->sendClientMessage(
                                     ERROR_COLOUR, u(fmt::format("Литры — целое число от 1 до {:.0f}",
                                                                 VehicleService::FUEL_CAPACITY)));
                                 showLitresInput(*driver, businessId);
                                 return;
                             }
                             refuel(*driver, businessId, static_cast<float>(litres));
                         });
}

void GasStationSystem::refuel(IPlayer &player, int businessId, float litres)
{
    const int playerId = player.getID();
    // Ре-валидация на клике: пока висел диалог, игрок мог выйти из машины, отъехать
    // от АЗС, а саму точку могли снести.
    if (m_stateService.getState(playerId) != PlayerState_Driver)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Заправлять можно только сидя за рулём"));
        return;
    }
    IVehicle *vehicle = m_vehicleService.getVehicle(playerId);
    if (!vehicle)
    {
        return;
    }
    if (stationAtPump(m_locationService.getPosition(playerId)) != businessId)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы отъехали от колонки"));
        return;
    }

    const int vehicleId = vehicle->getID();
    // Заливаем только то, что влезет: платить за перелив игрок не должен.
    const float free = std::max(0.0f, VehicleService::FUEL_CAPACITY - m_vehicleService.getFuel(vehicleId));
    // И только то, что есть в резервуаре станции: больше налить неоткуда.
    const int inTank = m_businessService.stockOf(businessId, STOCK_FUEL);
    const float poured = std::min({litres, free, static_cast<float>(inTank)});
    if (inTank <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("На этой АЗС кончился бензин"));
        return;
    }
    if (poured < MIN_REFUEL_LITRES)
    {
        player.sendClientMessage(INFO_COLOUR, u("Бак уже полный"));
        return;
    }

    // Литры целые: склад считается штуками, и продать «полтора литра» из резервуара
    // нельзя — иначе остаток на табличке разъедется с реально проданным.
    const int litresToSell = static_cast<int>(std::floor(poured));
    const std::int64_t price = priceOf(static_cast<float>(litresToSell));
    // Деньги забираем ДО долива: обратный порядок налил бы бесплатно при отказе.
    if (!m_moneyService.take(player, static_cast<unsigned long long>(price)))
    {
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Не хватает денег: нужно {}", Money::text(price))));
        return;
    }
    // Резервуар списываем ПОСЛЕ денег, но ДО долива: последние литры могли забрать,
    // пока игрок читал диалог.
    if (!m_businessService.consumeStock(businessId, STOCK_FUEL, litresToSell))
    {
        m_moneyService.giveMoney(player, static_cast<unsigned long long>(price));
        player.sendClientMessage(ERROR_COLOUR, u("Бензин на станции только что закончился"));
        return;
    }
    m_vehicleService.refuel(*vehicle, static_cast<float>(litresToSell));

    // Выручка — в копилку АЗС. У ничейной точки копилки нет: заправка всё равно
    // работает (иначе мир встанет), деньги просто уходят из экономики.
    m_businessService.addIncome(businessId, price);

    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Залито {} л за {}. В баке {:.0f} л. В резервуаре станции {} л",
                                           litresToSell, Money::text(price), m_vehicleService.getFuel(vehicleId),
                                           m_businessService.stockOf(businessId, STOCK_FUEL))));
}
