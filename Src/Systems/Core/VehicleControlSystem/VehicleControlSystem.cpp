#include "Systems/Core/VehicleControlSystem/VehicleControlSystem.h"

#include "Utils/Encoding/Encoding.h"

namespace
{
const Colour ERROR_COLOUR{255, 90, 90};

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

VehicleControlSystem::VehicleControlSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_keyService(serviceRegister.getService<PlayerKeyService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>())
{
    // Подписки на фронт клавиш. Антиспам повторов уже внутри PlayerKeyService.
    // Двигатель — Action: в машине этот бит ставит левый Ctrl (а также ALT GR / NUM0).
    m_keyService.onPress(PlayerKeyService::Key::Action, [this](IPlayer &player) { toggleEngine(player); });
    // Фары — Fire: в машине этот бит ставят ЛКМ и левый Alt (drive-by огонь).
    m_keyService.onPress(PlayerKeyService::Key::Fire, [this](IPlayer &player) { toggleLights(player); });

    // Момент опустошения бака ПОД ВОДИТЕЛЕМ (secondTick VehicleService, не
    // per-tick) — Core оповещает фактом, текст шлём здесь. driverId — серверный
    // (getDriver на момент опустошения), не клиентский ввод.
    m_vehicleService.subscribeFuelEmpty(
        [this](int /*vehicleId*/, int driverId)
        {
            IPlayer *player = m_core.getPlayers().get(driverId);
            if (!player)
                return; // водитель уже вышел — страховка от гонки колбэка/дисконнекта
            player->sendClientMessage(ERROR_COLOUR, u("Бак пуст — двигатель заглох"));
        });
}

IVehicle *VehicleControlSystem::drivenVehicle(IPlayer &player) const
{
    const int playerId = player.getID();
    // Гейтинг по серверной правде: переключать состояние можно ТОЛЬКО машины, за
    // рулём которой игрок реально сидит. getVehicle отдаёт машину по принятому
    // стейту, getSeat == 0 — водитель. Чужую/пустую машину тронуть нельзя.
    if (m_vehicleService.getSeat(playerId) != 0)
    {
        return nullptr;
    }
    IVehicle *vehicle = m_vehicleService.getVehicle(playerId);
    // Сверка обратного индекса: occupant-слот мог устареть (машину уничтожили, её
    // id переиспользовала другая) до обновления стейта игрока. Серверная правда о
    // водителе машины — getDriver; переключаем только при совпадении.
    if (!vehicle || m_vehicleService.getDriver(vehicle->getID()) != playerId)
    {
        return nullptr;
    }
    return vehicle;
}

void VehicleControlSystem::toggleEngine(IPlayer &player)
{
    IVehicle *vehicle = drivenVehicle(player);
    if (!vehicle)
    {
        return;
    }

    // Текущее значение: -1 (клиентский авто-режим) трактуем как «работает» —
    // заведённая машина это норма, первое нажатие её глушит. Заглохшую setEngine
    // не заведёт (сначала repair) — тоггл это уважает автоматически.
    const int8_t engine = vehicle->getParams().engine;
    const bool on = engine != 0; // -1 (авто) и 1 — считаем включённым
    const bool wantsStart = !on; // нажатие пытается ЗАВЕСТИ, а не заглушить

    // Причина будущего отказа спрашивается ДО setEngine (Core-сервис сам текста не
    // шлёт) — только на попытке завести: заглушить можно всегда. isStalled — раньше
    // outOfFuel (машина может быть и добита, и с пустым баком одновременно — стол
    // важнее для игрока, это про сам корпус, а не про расход). Антиспам — сам
    // фронт клавиши (PlayerKeyService), строка раз на нажатие.
    const int vehicleId = vehicle->getID();
    if (wantsStart && m_vehicleService.isStalled(vehicleId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Двигатель не заводится - машина слишком разбита"));
        return;
    }
    if (wantsStart && m_vehicleService.isOutOfFuel(vehicleId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Двигатель не заводится - бак пуст"));
        return;
    }

    m_vehicleService.setEngine(*vehicle, !on);
}

void VehicleControlSystem::toggleLights(IPlayer &player)
{
    IVehicle *vehicle = drivenVehicle(player);
    if (!vehicle)
    {
        return;
    }

    // -1 (авто) для фар трактуем как «выключены»: первое нажатие их включает.
    const int8_t lights = vehicle->getParams().lights;
    const bool on = lights == 1; // только явная 1 — включено; -1 и 0 — выключено
    m_vehicleService.setLights(*vehicle, !on);
}
