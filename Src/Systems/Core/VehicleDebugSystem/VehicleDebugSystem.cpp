#include "Systems/Core/VehicleDebugSystem/VehicleDebugSystem.h"

#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/VehicleService/VehicleModelNames.h"
#include "Utils/Encoding/Encoding.h"
#include <array>
#include <cmath>
#include <fmt/format.h>
#include <string_view>
#include <vector>

namespace
{
const Colour DEBUG_COLOUR{170, 255, 170};

// Модель машины владельца Work для дев-спавна по умолчанию.
constexpr int DEV_WORK_MODEL = 411; // Infernus

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

// --- /vtune: русские подписи слотов/компонентов/цветов (бизнес-слой, не Core) ---

// Индекс — значение VehicleComponentSlot (0..15, SDK vehicles.hpp). Порядок и
// значения фиксированы ядром, менять нельзя.
constexpr std::array<const char *, 16> SLOT_LABELS = {
    "Спойлер",  "Капот",   "Крыша",     "Пороги",         "Фары",           "Нитро",
    "Выхлоп",   "Колёса",  "Стерео",    "Гидравлика",     "Передний бампер", "Задний бампер",
    "Вентиляционная решётка справа", "Вентиляционная решётка слева", "Передний кенгурятник",
    "Задний кенгурятник"};

std::string_view slotLabel(int slot)
{
    if (slot < 0 || static_cast<std::size_t>(slot) >= SLOT_LABELS.size())
        return "?";
    return SLOT_LABELS[static_cast<std::size_t>(slot)];
}

// Куратские имена для компонентов с осмысленным человеческим названием. Компактно
// намеренно — остальное падает в generic-фоллбек ниже; дизайнер расширит по вкусу.
std::string_view curatedComponentName(int component)
{
    switch (component)
    {
    case 1009:
        return "Нитро x2";
    case 1008:
        return "Нитро x5";
    case 1010:
        return "Нитро x10";
    case 1087:
        return "Гидравлика";
    case 1086:
        return "Стерео";
    default:
        return {};
    }
}

// Подпись пункта под-меню слота: куратское имя либо «Вариант N (id)» — position
// 1-based позиция компонента в списке ЭТОГО слота (не глобальный индекс).
std::string componentLabel(int component, int position)
{
    const std::string_view curated = curatedComponentName(component);
    if (!curated.empty())
        return std::string(curated);
    return fmt::format("Вариант {} ({})", position, component);
}

// Подпись «что сейчас стоит» в корневом меню: 0/отрицательное — пусто.
std::string installedLabel(int component)
{
    if (component <= 0)
        return "пусто";
    const std::string_view curated = curatedComponentName(component);
    if (!curated.empty())
        return std::string(curated);
    return fmt::format("id {}", component);
}

// Кураторская палитра цвета машины (colour1==colour2, одиночный тон): id —
// источник правды (SetVehicleColor 0..255 на канал), названы уверенно только
// 0/1 (чёрный/белый) — точной таблицы оттенков SA для остальных id в репозитории
// нет, подписи «Цвет N» рабочие до визуальной сверки дизайнером (см. Docs/Vehicles.md).
constexpr std::array<int, 16> COLOUR_PALETTE = {0,  1,  3,  6,  10,  16,  26,  36,
                                                46, 56, 66, 86, 106, 126, 146, 166};

std::string colourLabel(int id)
{
    if (id == 0)
        return "Чёрный";
    if (id == 1)
        return "Белый";
    return fmt::format("Цвет {}", id);
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
                     if (model < VehicleModelNames::MIN_MODEL || model > VehicleModelNames::MAX_MODEL)
                         model = 411; // Infernus по умолчанию

                     // Создание — через единый API VehicleService; тестовая машина без
                     // владельца. Интерьер выставляем после (в сигнатуре create его нет).
                     const Vector3 position =
                         m_locationService.getPosition(player.getID()) + Vector3(3.0f, 3.0f, 0.5f);
                     // Ориентация машины = поворот игрока (yaw); не-конечный (NaN/Inf) -> 0.
                     const float yaw = player.getRotation().ToEuler().z;
                     const float angle = std::isfinite(yaw) ? yaw : 0.0f;
                     IVehicle *vehicle = m_vehicleService.create(model, position, angle, -1, -1,
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

    commands.add("vtune", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showTuneRoot(player); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL),
                 "меню тюнинга своей машины: компоненты, цвет, пейнтджоб", PlayerCommandService::HelpCategory::Hidden);

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

IVehicle *VehicleDebugSystem::drivenVehicle(IPlayer &player)
{
    if (m_vehicleService.getSeat(player.getID()) != 0)
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Сядьте за руль этой машины"));
        return nullptr;
    }
    return currentVehicle(player);
}

void VehicleDebugSystem::showDevMenu(IPlayer &player)
{
    std::string body;
    body += "Заспавнить тачку рядом (владелец Work)\n";
    body += "Заправить мою тачку (до полного)\n";
    body += "Показать топливо моей машины\n";
    body += "Взорвать текущую машину";

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
            case 3: // взорвать машину, в которой сидит дев (тест реального уничтожения)
            {
                IVehicle *vehicle = m_vehicleService.getVehicle(playerId);
                if (!vehicle)
                {
                    player->sendClientMessage(DEBUG_COLOUR, u("Вы не в машине"));
                    return;
                }
                // explode форсирует смерть через клиента ВОДИТЕЛЯ (его driver-sync
                // репортит Health<=0). Пассажир детонацию не вызовет — гейтим по рулю,
                // чтобы не показывать ложное «взорвана».
                if (m_vehicleService.getSeat(playerId) != 0)
                {
                    player->sendClientMessage(DEBUG_COLOUR, u("Сядьте за руль — взрыв форсируется только из-за руля"));
                    return;
                }
                // Привилегированная серверная операция: форсирует смерть (HP -> 0) мимо
                // анти-грифинга, с серверной санкцией (serverKilled). Клиент детонирует ->
                // onVehicleDeath -> died-политика -> личная пропадает.
                m_vehicleService.explode(*vehicle);
                player->sendClientMessage(DEBUG_COLOUR, u("Машина взорвана"));
                break;
            }
            default:
                break;
            }
        });
}

IVehicle *VehicleDebugSystem::tuneVehicle(IPlayer &player, int vehicleId)
{
    // drivenVehicle сам шлёт сообщение и вернёт nullptr, если дев не в машине или
    // не за рулём (currentVehicle/drivenVehicle). Отдельная проверка id нужна для
    // случая «дев пересел в ДРУГУЮ свою машину, оставаясь за рулём» — диалог висел
    // на СТАРОЙ, тюнить новую по старому id нельзя.
    IVehicle *vehicle = drivenVehicle(player);
    if (!vehicle)
        return nullptr;
    if (vehicle->getID() != vehicleId)
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Машина сменилась — откройте /vtune заново"));
        return nullptr;
    }
    return vehicle;
}

void VehicleDebugSystem::showTuneRoot(IPlayer &player)
{
    IVehicle *vehicle = drivenVehicle(player);
    if (!vehicle)
        return;
    const int vehicleId = vehicle->getID();

    // Пункт на КАЖДЫЙ слот (видим всегда — недоступность слота для модели
    // объясняет клик, не скрытие), затем «Цвет» и «Пейнтджоб».
    std::string body;
    for (int slot = 0; slot < VehicleService::COMPONENT_SLOT_COUNT; ++slot)
    {
        const int installed = m_vehicleService.installedInSlot(vehicleId, slot);
        body += fmt::format("{} — {}\n", slotLabel(slot), installedLabel(installed));
    }
    body += "Цвет\n";
    body += "Пейнтджоб";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Тюнинг — дев-меню", body, "Выбрать", "Закрыть"),
        [this, playerId = player.getID(), vehicleId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return; // игрок ушёл или закрыл меню

            IVehicle *vehicle = tuneVehicle(*player, vehicleId);
            if (!vehicle)
                return;

            if (listItem >= 0 && listItem < VehicleService::COMPONENT_SLOT_COUNT)
            {
                showTuneSlot(*player, vehicleId, listItem);
            }
            else if (listItem == VehicleService::COMPONENT_SLOT_COUNT)
            {
                showTuneColour(*player, vehicleId);
            }
            else if (listItem == VehicleService::COMPONENT_SLOT_COUNT + 1)
            {
                showTunePaintJob(*player, vehicleId);
            }
        });
}

void VehicleDebugSystem::showTuneSlot(IPlayer &player, int vehicleId, int slot)
{
    IVehicle *vehicle = tuneVehicle(player, vehicleId);
    if (!vehicle)
        return;

    std::vector<int> components;
    m_vehicleService.componentsForSlot(vehicle->getModel(), slot, components);
    if (components.empty())
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Для этой модели нет деталей этого слота"));
        showTuneRoot(player);
        return;
    }

    std::string body = "Снять\n";
    for (std::size_t i = 0; i < components.size(); ++i)
        body += componentLabel(components[i], static_cast<int>(i) + 1) + "\n";
    body.pop_back(); // убрать хвостовой '\n'

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, std::string(slotLabel(slot)), body, "Выбрать", "Назад"),
        [this, playerId = player.getID(), vehicleId, slot](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showTuneRoot(*player);
                return;
            }

            IVehicle *vehicle = tuneVehicle(*player, vehicleId);
            if (!vehicle)
                return;

            if (listItem == 0) // «Снять»
            {
                const int installed = m_vehicleService.installedInSlot(vehicleId, slot);
                if (installed <= 0)
                {
                    player->sendClientMessage(DEBUG_COLOUR, u("Слот уже пуст"));
                }
                else
                {
                    m_vehicleService.removeComponent(*vehicle, installed);
                    m_vehicleService.notifyServerTuned(*vehicle);
                    player->sendClientMessage(DEBUG_COLOUR, u("Деталь снята"));
                }
                showTuneSlot(*player, vehicleId, slot);
                return;
            }

            // Ре-сверка списка на клике (диалог мог провисеть) — тот же паттерн, что
            // CarMenuSystem::buildActions + сверка выбора с актуальным набором.
            std::vector<int> current;
            m_vehicleService.componentsForSlot(vehicle->getModel(), slot, current);
            const std::size_t index = listItem >= 1 ? static_cast<std::size_t>(listItem - 1) : current.size();
            if (index >= current.size())
            {
                showTuneSlot(*player, vehicleId, slot); // список изменился/мусорный listItem — перепоказать
                return;
            }

            const int component = current[index];
            m_vehicleService.installComponent(*vehicle, component); // сам валидирует модель/диапазон — вторая линия
            m_vehicleService.notifyServerTuned(*vehicle);
            player->sendClientMessage(DEBUG_COLOUR,
                                      u(fmt::format("Установлено: {}", componentLabel(component, listItem))));
            showTuneSlot(*player, vehicleId, slot);
        });
}

void VehicleDebugSystem::showTuneColour(IPlayer &player, int vehicleId)
{
    IVehicle *vehicle = tuneVehicle(player, vehicleId);
    if (!vehicle)
        return;

    std::string body;
    for (const int id : COLOUR_PALETTE)
        body += colourLabel(id) + "\n";
    body += "Ввести код цвета";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Цвет машины", body, "Выбрать", "Назад"),
        [this, playerId = player.getID(), vehicleId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showTuneRoot(*player);
                return;
            }

            IVehicle *vehicle = tuneVehicle(*player, vehicleId);
            if (!vehicle)
                return;

            if (listItem == static_cast<int>(COLOUR_PALETTE.size()))
            {
                showTuneColourInput(*player, vehicleId);
                return;
            }
            if (listItem < 0 || static_cast<std::size_t>(listItem) >= COLOUR_PALETTE.size())
            {
                showTuneColour(*player, vehicleId);
                return;
            }

            const int colour = COLOUR_PALETTE[static_cast<std::size_t>(listItem)];
            m_vehicleService.setColour(*vehicle, colour, colour);
            m_vehicleService.notifyServerTuned(*vehicle);
            player->sendClientMessage(DEBUG_COLOUR, u(fmt::format("Цвет: {}", colourLabel(colour))));
            showTuneColour(*player, vehicleId);
        });
}

void VehicleDebugSystem::showTunePaintJob(IPlayer &player, int vehicleId)
{
    IVehicle *vehicle = tuneVehicle(player, vehicleId);
    if (!vehicle)
        return;

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Пейнтджоб", "Нет (0)\n1\n2", "Выбрать", "Назад"),
        [this, playerId = player.getID(), vehicleId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showTuneRoot(*player);
                return;
            }

            IVehicle *vehicle = tuneVehicle(*player, vehicleId);
            if (!vehicle)
                return;
            if (listItem < 0 || listItem > 2)
            {
                showTunePaintJob(*player, vehicleId);
                return;
            }

            m_vehicleService.setPaintJob(*vehicle, listItem);
            m_vehicleService.notifyServerTuned(*vehicle);
            // Подтверждаем по факту: SDK молча игнорирует пейнтджоб, невалидный для
            // модели (no-op без ошибки) — сверяемся с реально применённым значением.
            if (m_vehicleService.getPaintJob(vehicleId) == listItem)
                player->sendClientMessage(
                    DEBUG_COLOUR,
                    u(listItem == 0 ? std::string("Пейнтджоб: Нет") : fmt::format("Пейнтджоб: {}", listItem)));
            else
                player->sendClientMessage(
                    DEBUG_COLOUR, u(fmt::format("Эта модель не поддерживает пейнтджоб {}", listItem)));
            showTunePaintJob(*player, vehicleId);
        });
}

void VehicleDebugSystem::showTuneColourInput(IPlayer &player, int vehicleId)
{
    IVehicle *vehicle = tuneVehicle(player, vehicleId);
    if (!vehicle)
        return;

    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Код цвета 1", "Введите код первого цвета. Допустимо от 0 до 255", "Далее",
                   "Назад"),
        [this, playerId = player.getID(), vehicleId](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                showTuneColour(*player, vehicleId);
                return;
            }
            if (value < 0 || value > 255)
            {
                player->sendClientMessage(DEBUG_COLOUR, u("Код цвета должен быть от 0 до 255"));
                showTuneColourInput(*player, vehicleId);
                return;
            }
            if (!tuneVehicle(*player, vehicleId))
                return;
            const int colour1 = static_cast<int>(value);

            m_dialogService.showNumberInput(
                *player,
                makeDialog(DialogStyle_INPUT, "Код цвета 2", "Введите код второго цвета. Допустимо от 0 до 255",
                           "Готово", "Назад"),
                [this, playerId, vehicleId, colour1](DialogResponse response2, std::int64_t value2)
                {
                    IPlayer *player = m_core.getPlayers().get(playerId);
                    if (!player)
                        return;
                    if (response2 != DialogResponse_Left)
                    {
                        showTuneColour(*player, vehicleId);
                        return;
                    }
                    if (value2 < 0 || value2 > 255)
                    {
                        player->sendClientMessage(DEBUG_COLOUR, u("Код цвета должен быть от 0 до 255"));
                        showTuneColourInput(*player, vehicleId);
                        return;
                    }
                    IVehicle *vehicle = tuneVehicle(*player, vehicleId);
                    if (!vehicle)
                        return;
                    const int colour2 = static_cast<int>(value2);
                    m_vehicleService.setColour(*vehicle, colour1, colour2);
                    m_vehicleService.notifyServerTuned(*vehicle);
                    player->sendClientMessage(DEBUG_COLOUR,
                                              u(fmt::format("Цвет {}/{} установлен", colour1, colour2)));
                    showTuneColour(*player, vehicleId);
                });
        });
}
