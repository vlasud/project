#include "Systems/VehicleNameSystem/VehicleNameSystem.h"

#include <chrono>
#include <string_view>

namespace
{
// Длительность попапа названия машины (SA-стиль 7, правый нижний угол).
constexpr std::chrono::milliseconds SHOW_TIME{1000};
// Стиль GameText для попапа названия машины. Классические стили SA (0..6)
// рендерятся на ВСЕХ клиентах (в т.ч. легаси 0.3.7); расширенные open.mp (7..15,
// напр. 7 «SA vehicle names») — только на open.mp-клиенте.
constexpr int VEHICLE_NAME_STYLE = 1;
} // namespace

VehicleNameSystem::VehicleNameSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_gameText(serviceRegister.getService<GameTextService>())
{
    core.getPlayers().getPlayerChangeDispatcher().addEventHandler(this);
}

void VehicleNameSystem::onPlayerStateChange(IPlayer &player, PlayerState newState, PlayerState oldState)
{
    // Только посадка за руль — пассажирам попап не показываем.
    if (newState != PlayerState_Driver)
    {
        return;
    }

    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }

    // Машина водителя из источника правды; null — рассинхрон стейта (машину уже
    // уничтожили, событие не дошло) — просто ничего не показываем.
    IVehicle *vehicle = m_vehicleService.getVehicle(playerId);
    if (!vehicle)
    {
        return;
    }

    const std::string_view name = m_vehicleService.getModelName(vehicle->getID());
    if (name.empty())
    {
        return;
    }

    // Имена каталога — ASCII (cp1251-идентичны), StringView не владеет строкой:
    // показываем без копии и конвертации. Санитизацию делает GameTextService.
    m_gameText.show(player, StringView(name.data(), name.size()), SHOW_TIME, VEHICLE_NAME_STYLE);
}
