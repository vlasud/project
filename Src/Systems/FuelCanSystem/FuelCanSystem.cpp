#include "Systems/FuelCanSystem/FuelCanSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/Geometry/Geometry.h"
#include <cmath>
#include <fmt/format.h>
#include <string>

namespace
{
// Цвета — в ряд с аптечкой и инструментами.
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// «Стоит у машины»: дистанция от игрока до ОРИГИНА машины. Тот же радиус, что у
// ремонта: заправляют от того же борта, что и чинят.
constexpr float REFUEL_RADIUS = 5.0f;

// «Смотрит на машину»: половина угла конуса, cos(45°).
constexpr float LOOK_MIN_COS = 0.7071f;

// Сколько длится заправка. Игрок всё это время свободен в движении — отойдёт, и
// проверка на финише просто не даст залить (замораживать управление нельзя).
constexpr Milliseconds REFUEL_DURATION{5000};

// Анимация возни у машины — та же, что у ремонта: присел и работает руками.
// Библиотеку обязательно предзагружать: первый ApplyAnimation из незагруженной либы
// не проигрывается.
const char *const REFUEL_ANIM_LIB = "BOMBER";
const char *const REFUEL_ANIM_NAME = "BOM_Plant_Loop";

// Допуск «бак полный»: топливо float, ровно CAPACITY бывает не всегда.
constexpr float FULL_FUEL_EPS = 0.5f;
} // namespace

FuelCanSystem::FuelCanSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_inventory(serviceRegister.getService<InventoryService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_gridService(serviceRegister.getService<GridService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_animationService(serviceRegister.getService<PlayerAnimationService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_timers(serviceRegister.getService<TimerService>())
{
    m_targetVehicle.fill(-1);

    // Регистрация типа предмета — как у аптечки и инструментов. Дев-выдача (/idev) и
    // инвентарь (/inv) подхватят новый тип сами.
    m_inventory.registerItem(ITEM_FUELCAN, "Канистра с бензином", FUELCAN_MAX);
    // Применение из инвентаря — та же механика, что и /refuel.
    m_inventory.setUseHandler(ITEM_FUELCAN, [this](IPlayer &player) { refuel(player); });

    // Либа анимации грузится заранее: первая заправка иначе прошла бы без анимации.
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            m_animationService.preloadLibrary(player, REFUEL_ANIM_LIB);
        });

    // Конец сессии: снять флаг и таймер — переиспользованный слот не должен
    // унаследовать чужую заправку.
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            const int playerId = player.getID();
            if (!validPlayerId(playerId))
            {
                return;
            }
            m_timers.cancel(m_pendingTimer[playerId]);
            m_refuelling[playerId] = false;
            m_targetVehicle[playerId] = -1;
        });

    serviceRegister.getService<PlayerCommandService>().add(
        "refuel", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            refuel(player);
        },
        {}, "залить бензин из канистры (стоя у машины)", PlayerCommandService::HelpCategory::Misc);
}

IVehicle *FuelCanSystem::targetVehicle(int playerId, std::string &reason) const
{
    const Vector3 position = m_locationService.getPosition(playerId);

    GridService::Result nearest;
    if (!m_gridService.closest(position, REFUEL_RADIUS, gridMask(GridEntityType::Vehicle), nearest))
    {
        reason = "Рядом нет машины — подойдите ближе";
        return nullptr;
    }
    IVehicle *vehicle = m_vehicleService.get(nearest.id);
    if (!vehicle)
    {
        reason = "Рядом нет машины — подойдите ближе";
        return nullptr;
    }

    // «Смотрит на машину»: направление взгляда против направления на машину, по XY.
    // Клиентский yaw — единственный источник факта «куда смотрит», зато сама ПОЗИЦИЯ
    // обоих серверная, поэтому «дотянуться» через карту нельзя.
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return nullptr;
    }
    const float yaw = player->getRotation().ToEuler().z;
    const Vector3 facing = Geometry::headingVector(yaw);
    const Vector3 delta = vehicle->getPosition() - position;
    const float flat = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (flat < 0.001f)
    {
        return vehicle; // стоит вплотную/внутри габарита — направление не определено
    }
    if ((facing.x * delta.x + facing.y * delta.y) / flat < LOOK_MIN_COS)
    {
        reason = "Повернитесь к машине — нужно смотреть на неё";
        return nullptr;
    }
    return vehicle;
}

int FuelCanSystem::refuellerOf(int vehicleId, int exceptPlayerId) const
{
    if (vehicleId < 0)
    {
        return -1;
    }
    for (int p = 0; p < MAX_PLAYERS; ++p)
    {
        if (p != exceptPlayerId && m_refuelling[p] && m_targetVehicle[p] == vehicleId)
        {
            return p;
        }
    }
    return -1;
}

void FuelCanSystem::refuel(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId))
    {
        return;
    }

    if (m_refuelling[playerId])
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы уже заправляете машину"));
        return;
    }
    // Заправляют снаружи: до горловины из салона не дотянуться. Стейт серверный.
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из машины, чтобы её заправить"));
        return;
    }

    std::string reason;
    IVehicle *vehicle = targetVehicle(playerId, reason);
    if (!vehicle)
    {
        player.sendClientMessage(ERROR_COLOUR, u(reason));
        return;
    }

    // Машину уже заправляют: второй долил бы в почти полный бак и потерял канистру.
    const int busy = refuellerOf(vehicle->getID(), playerId);
    if (busy >= 0)
    {
        IPlayer *other = m_core.getPlayers().get(busy);
        const std::string name =
            other ? Encoding::neutralizeColorCodes(
                        std::string_view(other->getName().data(), other->getName().size()))
                  : std::string("другой игрок");
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Эту машину уже заправляет {}[{}]", name, busy)));
        return;
    }

    // Полный бак не доливаем — иначе канистра сгорела бы впустую (тот же порядок
    // проверок, что у аптечки и инструментов: сперва «не нужно», потом «нечем»).
    if (m_vehicleService.getFuel(vehicle->getID()) >= VehicleService::FUEL_CAPACITY - FULL_FUEL_EPS)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Бак полный — заливать некуда"));
        return;
    }
    if (m_inventory.count(playerId, ITEM_FUELCAN) == 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас нет канистры"));
        return;
    }

    m_refuelling[playerId] = true;
    m_targetVehicle[playerId] = vehicle->getID();

    // Прерываемая анимация: игрок волен уйти, сервер его не держит (замораживать
    // управление в проекте запрещено). Уход поймает проверка на финише.
    m_animationService.play(player, AnimationData(4.1f, true, false, false, false, 0, REFUEL_ANIM_LIB,
                                                  REFUEL_ANIM_NAME),
                            true);
    player.sendClientMessage(INFO_COLOUR, u("Вы принялись заливать бензин..."));

    m_pendingTimer[playerId] = m_timers.setPlayerTimeout(player, REFUEL_DURATION,
                                                         [this](IPlayer &p)
                                                         {
                                                             finishRefuel(p);
                                                         });
}

void FuelCanSystem::finishRefuel(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId) || !m_refuelling[playerId])
    {
        return;
    }
    const int targetId = m_targetVehicle[playerId];
    m_refuelling[playerId] = false;
    m_targetVehicle[playerId] = -1;
    m_animationService.stop(player);

    // Ре-валидация: пяти секунд достаточно, чтобы отойти, сесть в машину или остаться
    // без канистры. Ничего из этого не должно заправлять машину.
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Заправка прервана — вы сели в машину"));
        return;
    }

    std::string reason;
    IVehicle *vehicle = targetVehicle(playerId, reason);
    if (!vehicle || vehicle->getID() != targetId)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Заправка прервана — вы отошли от машины"));
        return;
    }
    if (m_inventory.count(playerId, ITEM_FUELCAN) == 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас нет канистры"));
        return;
    }
    // Бак могли залить, пока шла анимация: канистру за это не тратим.
    const float before = m_vehicleService.getFuel(vehicle->getID());
    if (before >= VehicleService::FUEL_CAPACITY - FULL_FUEL_EPS)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Бак уже полный — заливать некуда"));
        return;
    }

    // Заливаем и только ПОТОМ списываем канистру — как инструмент списывается по
    // факту починки. refuel сам клампит по объёму бака и снимает «заглохла».
    m_vehicleService.refuel(*vehicle, FUEL_PER_CAN);
    m_inventory.remove(playerId, ITEM_FUELCAN, 1);

    const float added = m_vehicleService.getFuel(vehicle->getID()) - before;
    const int left = m_inventory.count(playerId, ITEM_FUELCAN);
    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Залито {:.0f} л. В баке {:.0f} л. Осталось канистр: {}", added,
                                           m_vehicleService.getFuel(vehicle->getID()), left)));
}
