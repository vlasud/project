#include "Systems/Core/VehicleDebugSystem/VehicleDebugSystem.h"

#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <cmath>
#include <fmt/format.h>

namespace
{
const Colour DEBUG_COLOUR{170, 255, 170};

// Модель машины владельца Work для дев-спавна по умолчанию.
constexpr int DEV_WORK_MODEL = 411; // Infernus

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

Dialog makeDialog(DialogStyle style, const std::string &title, const std::string &body, const std::string &leftButton,
                  const std::string &rightButton)
{
    Dialog dialog;
    dialog.style = style;
    dialog.title = u(title);
    dialog.body = u(body);
    dialog.leftButton = u(leftButton);
    dialog.rightButton = u(rightButton);
    return dialog;
}
} // namespace

VehicleDebugSystem::VehicleDebugSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_gridService(serviceRegister.getService<GridService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add("veh", {{PlayerCommandService::Param::Int, "модель"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     int model = args.getInt(0);
                     if (model < 400 || model > 611)
                         model = 411; // Infernus по умолчанию

                     // Создание — через единый API VehicleService; тестовая машина без
                     // владельца. Интерьер выставляем после (в сигнатуре create его нет).
                     const Vector3 position =
                         m_locationService.getPosition(player.getID()) + Vector3(3.0f, 3.0f, 0.5f);
                     IVehicle *vehicle = m_vehicleService.create(model, position, 0.0f, -1, -1,
                                                                 VehicleService::Owner::None, -1);
                     if (!vehicle)
                     {
                         player.sendClientMessage(DEBUG_COLOUR, u("Пул машин переполнен"));
                         return;
                     }
                     vehicle->setVirtualWorld(m_locationService.getVirtualWorld(player.getID()));
                     vehicle->setInterior(static_cast<int>(m_locationService.getInterior(player.getID())));

                     // Серверная посадка — заодно проверка санкции стейта (без StateHack);
                     // в VehicleService машина уже попала через пул-событие создания.
                     m_stateService.putInVehicle(player, *vehicle, 0);
                     player.sendClientMessage(
                         DEBUG_COLOUR, u(fmt::format("Машина {} (id {}). Посадка серверная — /violations должен "
                                                     "быть пуст",
                                                     model, vehicle->getID())));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "заспавнить машину по модели и сесть в неё",
                 PlayerCommandService::HelpCategory::Hidden);

    commands.add("vput", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     GridService::Result nearest;
                     if (!m_gridService.closest(m_locationService.getPosition(player.getID()), 50.0f,
                                                gridMask(GridEntityType::Vehicle), nearest))
                     {
                         player.sendClientMessage(DEBUG_COLOUR, u("Рядом нет машин (50 м)"));
                         return;
                     }
                     IVehicle *vehicle = m_vehicleService.get(nearest.id);
                     if (!vehicle)
                     {
                         player.sendClientMessage(DEBUG_COLOUR, u("Машина из сетки не найдена в пуле"));
                         return;
                     }
                     m_stateService.putInVehicle(player, *vehicle, 0);
                     player.sendClientMessage(DEBUG_COLOUR,
                                              u(fmt::format("Посажен в машину {} ({:.1f} м)", nearest.id,
                                                            std::sqrt(nearest.distSq))));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "сесть в ближайшую машину (до 50 м)",
                 PlayerCommandService::HelpCategory::Hidden);

    commands.add("vdel", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     const int id = vehicle->getID();
                     m_stateService.removeFromVehicle(player);
                     m_vehicleService.destroy(id);
                     player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Машина {} удалена", id)));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "удалить машину, в которой сидишь",
                 PlayerCommandService::HelpCategory::Hidden);

    commands.add("vrespawn", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     m_stateService.removeFromVehicle(player);
                     vehicle->respawn();
                     player.sendClientMessage(DEBUG_COLOUR, u("Машина переспавнена (HP снова 1000)"));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "переспавнить машину (HP → 1000)",
                 PlayerCommandService::HelpCategory::Hidden);

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
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL),
                 "информация о машине: HP сервер/клиент, посадочное место",
                 PlayerCommandService::HelpCategory::Hidden);

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
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "задать HP машины (0–1000)",
                 PlayerCommandService::HelpCategory::Hidden);

    commands.add("vrepair", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     m_vehicleService.repair(*vehicle);
                     player.sendClientMessage(DEBUG_COLOUR, u("Отремонтирована (серверно — без нарушений)"));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "отремонтировать машину",
                 PlayerCommandService::HelpCategory::Hidden);

    commands.add("vengine", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     const bool on = vehicle->getParams().engine != 1;
                     m_vehicleService.setEngine(*vehicle, on);
                     player.sendClientMessage(DEBUG_COLOUR, u(on ? "Двигатель: ВКЛ" : "Двигатель: выкл"));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "завести или заглушить двигатель",
                 PlayerCommandService::HelpCategory::Hidden);

    commands.add("vlock", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     IVehicle *vehicle = currentVehicle(player);
                     if (!vehicle)
                         return;
                     const bool lock = vehicle->getParams().doors != 1;
                     m_vehicleService.setLocked(*vehicle, lock);
                     player.sendClientMessage(DEBUG_COLOUR, u(lock ? "Двери: ЗАПЕРТЫ" : "Двери: открыты"));
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "запереть или отпереть двери машины",
                 PlayerCommandService::HelpCategory::Hidden);

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
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "установить тюнинг-компонент",
                 PlayerCommandService::HelpCategory::Hidden);

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
                 },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "тест анти-чита: имитировать чит ремонта машины",
                 PlayerCommandService::HelpCategory::Hidden);

    commands.add("vdev", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showDevMenu(player); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL),
                 "дев-меню машин: спавн (create+owner) и заправка", PlayerCommandService::HelpCategory::Hidden);
}

void VehicleDebugSystem::initialize(IComponentList * /*components*/)
{
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

void VehicleDebugSystem::showDevMenu(IPlayer &player)
{
    std::string body;
    body += "Заспавнить тачку рядом (владелец Work)\n";
    body += "Заправить мою тачку (до полного)\n";
    body += "Показать топливо моей машины";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Дев-меню машин", body, "Выбрать", "Закрыть"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return; // игрок ушёл или закрыл меню
            }

            switch (listItem)
            {
            case 0: // create() машины владельца Work рядом с девом
            {
                const Vector3 position =
                    m_locationService.getPosition(playerId) + Vector3(3.0f, 3.0f, 0.5f);
                IVehicle *vehicle = m_vehicleService.create(DEV_WORK_MODEL, position, 0.0f, -1, -1,
                                                            VehicleService::Owner::Work, playerId);
                if (!vehicle)
                {
                    player->sendClientMessage(DEBUG_COLOUR, u("Пул машин переполнен"));
                    return;
                }
                vehicle->setVirtualWorld(m_locationService.getVirtualWorld(playerId));
                vehicle->setInterior(static_cast<int>(m_locationService.getInterior(playerId)));
                player->sendClientMessage(
                    DEBUG_COLOUR, u(fmt::format("Машина {} (id {}) создана. Владелец Work #{}, бак полный.",
                                                DEV_WORK_MODEL, vehicle->getID(), playerId)));
                break;
            }
            case 1: // refuel машины, в которой сидит дев
            {
                IVehicle *vehicle = m_vehicleService.getVehicle(playerId);
                if (!vehicle)
                {
                    player->sendClientMessage(DEBUG_COLOUR, u("Вы не в машине"));
                    return;
                }
                m_vehicleService.refuel(*vehicle, VehicleService::FUEL_CAPACITY);
                player->sendClientMessage(
                    DEBUG_COLOUR, u(fmt::format("Бак заправлен: {:.0f}/{:.0f}. Заводите двигатель (Fire).",
                                                m_vehicleService.getFuel(vehicle->getID()),
                                                VehicleService::FUEL_CAPACITY)));
                break;
            }
            case 2: // показать топливо машины, в которой сидит дев
            {
                IVehicle *vehicle = m_vehicleService.getVehicle(playerId);
                if (!vehicle)
                {
                    player->sendClientMessage(DEBUG_COLOUR, u("Вы не в машине"));
                    return;
                }
                const int vehicleId = vehicle->getID();
                player->sendClientMessage(
                    DEBUG_COLOUR,
                    u(fmt::format("Топливо: {:.1f}/{:.0f}{}", m_vehicleService.getFuel(vehicleId),
                                  VehicleService::FUEL_CAPACITY,
                                  m_vehicleService.isOutOfFuel(vehicleId) ? " — БАК ПУСТ" : "")));
                break;
            }
            default:
                break;
            }
        });
}
