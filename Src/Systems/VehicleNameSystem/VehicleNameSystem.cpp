#include "Systems/VehicleNameSystem/VehicleNameSystem.h"

#include <chrono>
#include <string_view>

namespace
{
// Длительность попапа названия машины (единый экранный попап ScreenNoticeService).
// 1.5с: выше MIN_DISPLAY сервиса — длинные имена (Tug Stairs Trailer) успевают
// зацепиться боковым зрением, но попап остаётся коротким акцентом, не задержкой.
constexpr std::chrono::milliseconds SHOW_TIME{1500};
} // namespace

VehicleNameSystem::VehicleNameSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_screenNotice(serviceRegister.getService<ScreenNoticeService>())
{
    listen(core.getPlayers().getPlayerChangeDispatcher(), this);
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
    // уничтожили, событие ещё не дошло) — просто ничего не показываем.
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
    // показываем без копии и конвертации. Санитизацию делает TextDrawService.
    m_screenNotice.show(player, StringView(name.data(), name.size()), SHOW_TIME, Colour::White());
}
