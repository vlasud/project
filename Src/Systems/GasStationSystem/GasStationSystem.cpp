#include "Systems/GasStationSystem/GasStationSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Systems/MedkitSystem/MedkitSystem.h"
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

// Радиус обслуживания вокруг ВХОДА в АЗС: колонок как отдельных объектов нет,
// заправочная площадка — круг вокруг точки бизнеса.
constexpr float SERVICE_RADIUS = 20.0f;

// Цена литра. Бак — 100 л, расход 0.1 л/сек, то есть полный бак ≈ 17 минут езды:
// заправка «под завязку» с сухого стоит 300$ за эти 17 минут. Величина
// балансная — правится одной строкой (см. Docs/GasStation.md).
constexpr std::int64_t FUEL_PRICE_PER_LITRE = 3;

// Ниже этого остатка доливать нечего — не гоняем диалог ради капли.
constexpr float MIN_REFUEL_LITRES = 1.0f;

// Пул интерьеров АЗС. Магазин при заправке в SA — тот же 24/7, отдельного
// интерьера у него нет; пул совпадает с магазинным, дев выбирает при создании.
std::vector<BusinessService::CatalogEntry> stationCatalog()
{
    return {
        {"АЗС 1", 17, Vector3(-25.72f, -187.82f, 1003.54f), 0.0f},
        {"АЗС 2", 10, Vector3(6.08f, -28.89f, 1003.54f), 0.0f},
        {"АЗС 3", 18, Vector3(-30.98f, -89.68f, 1003.54f), 0.0f},
    };
}

// Ассортимент лавки при заправке — тот же, что в 24/7 (см. Docs/Inventory.md).
std::vector<BusinessShop::Good> stationGoods()
{
    return {
        {MedkitSystem::ITEM_MEDKIT, 250},
        {ToolkitSystem::ITEM_TOOLKIT, 400},
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
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_shop(core, serviceRegister, "Лавка при АЗС", stationGoods())
{
    m_businessService.registerType(BusinessService::Type::GasStation, "АЗС", stationCatalog(),
                                   [this](IPlayer &player, int businessId)
                                   {
                                       m_shop.show(player, businessId);
                                   });

    serviceRegister.getService<PlayerCommandService>().add(
        "fuel", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            showFuelMenu(player);
        },
        {}, "заправить машину на АЗС", PlayerCommandService::HelpCategory::Economy);
}

int GasStationSystem::stationNear(const Vector3 &position) const
{
    // Линейно по бизнесам (их десятки) на холодном пути — команда игрока, не тик.
    int nearest = -1;
    float best = SERVICE_RADIUS * SERVICE_RADIUS;
    for (const auto &[id, business] : m_businessService.businesses())
    {
        if (business.type != BusinessService::Type::GasStation)
        {
            continue;
        }
        const float dx = business.entrance.x - position.x;
        const float dy = business.entrance.y - position.y;
        const float dz = business.entrance.z - position.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        if (distance <= best)
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
    const int businessId = stationNear(m_locationService.getPosition(playerId));
    if (businessId < 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Рядом нет АЗС"));
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
    if (stationNear(m_locationService.getPosition(playerId)) != businessId)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы отъехали от АЗС"));
        return;
    }

    const int vehicleId = vehicle->getID();
    // Заливаем только то, что влезет: платить за перелив игрок не должен.
    const float free = std::max(0.0f, VehicleService::FUEL_CAPACITY - m_vehicleService.getFuel(vehicleId));
    const float poured = std::min(litres, free);
    if (poured < MIN_REFUEL_LITRES)
    {
        player.sendClientMessage(INFO_COLOUR, u("Бак уже полный"));
        return;
    }

    const std::int64_t price = priceOf(poured);
    // Деньги забираем ДО долива: обратный порядок налил бы бесплатно при отказе.
    if (!m_moneyService.take(player, static_cast<unsigned long long>(price)))
    {
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Не хватает денег: нужно {}", Money::text(price))));
        return;
    }
    m_vehicleService.refuel(*vehicle, poured);

    // Выручка — в копилку АЗС. У ничейной точки копилки нет: заправка всё равно
    // работает (иначе мир встанет), деньги просто уходят из экономики. Заправка
    // владельцем на своей же АЗС копилку не пополняет — иначе он ездит бесплатно
    // (см. BusinessService::addIncomeFrom).
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    m_businessService.addIncomeFrom(businessId, price,
                                    session ? std::to_string(session->accountId) : std::string());

    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Залито {:.0f} л за {}. В баке {:.0f} л", poured,
                                                        Money::text(price), m_vehicleService.getFuel(vehicleId))));
}
