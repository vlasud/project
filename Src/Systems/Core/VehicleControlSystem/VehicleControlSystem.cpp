#include "Systems/Core/VehicleControlSystem/VehicleControlSystem.h"

VehicleControlSystem::VehicleControlSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_keyService(serviceRegister.getService<PlayerKeyService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>())
{
    // Подписки на фронт клавиш. Антиспам повторов уже внутри PlayerKeyService.
    // Двигатель — Fire: в машине этот бит ставит левый Ctrl.
    m_keyService.onPress(PlayerKeyService::Key::Fire, [this](IPlayer &player) { toggleEngine(player); });
    // Фары — Action: в машине этот бит ставит ЛКМ (в машине ЛКМ это не Fire).
    m_keyService.onPress(PlayerKeyService::Key::Action, [this](IPlayer &player) { toggleLights(player); });
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
