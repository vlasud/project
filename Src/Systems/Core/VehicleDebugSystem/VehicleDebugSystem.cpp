#include "Systems/Core/VehicleDebugSystem/VehicleDebugSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <cmath>
#include <fmt/format.h>

namespace
{
const Colour DEBUG_COLOUR{170, 255, 170};

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

VehicleDebugSystem::VehicleDebugSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_gridService(serviceRegister.getService<GridService>())
{
    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add("veh", {{PlayerCommandService::Param::Int, "модель"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     if (!m_vehicles)
                     {
                         player.sendClientMessage(DEBUG_COLOUR, u("Компонент машин недоступен"));
                         return;
                     }
                     int model = args.getInt(0);
                     if (model < 400 || model > 611)
                         model = 411; // Infernus по умолчанию

                     VehicleSpawnData data;
                     data.respawnDelay = std::chrono::seconds(60);
                     data.modelID = model;
                     data.position = m_locationService.getPosition(player.getID()) + Vector3(3.0f, 3.0f, 0.5f);
                     data.zRotation = 0.0f;
                     data.colour1 = -1; // случайные цвета
                     data.colour2 = -1;
                     data.siren = false;
                     data.interior = m_locationService.getInterior(player.getID());

                     IVehicle *vehicle = m_vehicles->create(data);
                     if (!vehicle)
                     {
                         player.sendClientMessage(DEBUG_COLOUR, u("Пул машин переполнен"));
                         return;
                     }
                     vehicle->setVirtualWorld(m_locationService.getVirtualWorld(player.getID()));

                     // Серверная посадка — заодно проверка санкции стейта (без StateHack);
                     // в VehicleService машина уже попала через пул-событие создания.
                     m_stateService.putInVehicle(player, *vehicle, 0);
                     player.sendClientMessage(
                         DEBUG_COLOUR, u(fmt::format("Машина {} (id {}). Посадка серверная — /violations должен "
                                                     "быть пуст",
                                                     model, vehicle->getID())));
                 });

    commands.add("vput", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     GridService::Result nearest;
                     if (!m_gridService.closest(m_locationService.getPosition(player.getID()), 50.0f,
                                                gridMask(GridEntityType::Vehicle), nearest) ||
                         !m_vehicles)
                     {
                         player.sendClientMessage(DEBUG_COLOUR, u("Рядом нет машин (50 м)"));
                         return;
                     }
                     IVehicle *vehicle = m_vehicles->get(nearest.id);
                     if (!vehicle)
                     {
                         player.sendClientMessage(DEBUG_COLOUR, u("Машина из сетки не найдена в пуле"));
                         return;
                     }
                     m_stateService.putInVehicle(player, *vehicle, 0);
                     player.sendClientMessage(DEBUG_COLOUR,
                                              u(fmt::format("Посажен в машину {} ({:.1f} м)", nearest.id,
                                                            std::sqrt(nearest.distSq))));
                 });

    commands.add("vdel", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle || !m_vehicles)
                         return;
                     const int id = vehicle->getID();
                     m_stateService.removeFromVehicle(player);
                     m_vehicles->release(id);
                     player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Машина {} удалена", id)));
                 });

    commands.add("vrespawn", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     m_stateService.removeFromVehicle(player);
                     vehicle->respawn();
                     player.sendClientMessage(DEBUG_COLOUR, u("Машина переспавнена (HP снова 1000)"));
                 });

    commands.add("vinfo", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     const int vehicleId = vehicle->getID();
                     const float serverHp = m_vehicleService.getHealth(vehicleId);
                     const float clientHp = vehicle->getHealth(); // намеренно: сравнение с заявлением клиента
                     player.sendClientMessage(
                         DEBUG_COLOUR,
                         u(fmt::format("Машина {} | место {} | HP сервер {:.0f} / клиент {:.0f} (расхождение {:.1f})",
                                       vehicleId, m_vehicleService.getSeat(player.getID()), serverHp, clientHp,
                                       clientHp - serverHp)));
                 });

    commands.add("vhp", {{PlayerCommandService::Param::Int, "hp"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     int hp = args.getInt(0);
                     if (hp < 0 || hp > 1000)
                         hp = 1000;
                     m_vehicleService.setHealth(*vehicle, static_cast<float>(hp));
                     player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("HP машины: {} (серверно)", hp)));
                 });

    commands.add("vrepair", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     m_vehicleService.repair(*vehicle);
                     player.sendClientMessage(DEBUG_COLOUR, u("Отремонтирована (серверно — без нарушений)"));
                 });

    commands.add("vengine", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     const bool on = vehicle->getParams().engine != 1;
                     m_vehicleService.setEngine(*vehicle, on);
                     player.sendClientMessage(DEBUG_COLOUR, u(on ? "Двигатель: ВКЛ" : "Двигатель: выкл"));
                 });

    commands.add("vlock", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     const bool lock = vehicle->getParams().doors != 1;
                     m_vehicleService.setLocked(*vehicle, lock);
                     player.sendClientMessage(DEBUG_COLOUR, u(lock ? "Двери: ЗАПЕРТЫ" : "Двери: открыты"));
                 });

    commands.add("vmod", {{PlayerCommandService::Param::Int, "компонент"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     int component = args.getInt(0);
                     if (component < 1000 || component > 1193)
                         component = 1010; // нитро x10 по умолчанию
                     // Серверный тюнинг через API — мод-шоп не требуется.
                     m_vehicleService.addComponent(*vehicle, component);
                     player.sendClientMessage(DEBUG_COLOUR,
                                              u(fmt::format("Компонент {} установлен серверно", component)));
                 });

    commands.add("vhack", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     // НАМЕРЕННО сырой setHealth мимо сервиса — имитация repair hack.
                     // Валидатор обязан откатить HP и записать VehicleHack.
                     vehicle->setHealth(1000.0f);
                     player.sendClientMessage(
                         DEBUG_COLOUR, u("Сырой setHealth(1000): жди отката HP и VehicleHack в /violations "
                                         "(сначала побей машину /vhp 400)"));
                 });
}

void VehicleDebugSystem::initialize(IComponentList *components)
{
    m_vehicles = components->queryComponent<IVehiclesComponent>();
}

IVehicle *VehicleDebugSystem::currentVehicle(IPlayer &player)
{
    IVehicle *vehicle = m_vehicleService.getVehicle(player.getID());
    if (!vehicle)
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Вы не в машине"));
    }
    return vehicle;
}
