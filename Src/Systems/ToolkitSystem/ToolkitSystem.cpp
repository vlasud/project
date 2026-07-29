#include "Systems/ToolkitSystem/ToolkitSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/Geometry/Geometry.h"
#include <cmath>
#include <fmt/format.h>
#include <string>

namespace
{
// Цвета — в ряд с аптечкой: успех информационно-голубой, отказ красный.
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// «Стоит у машины»: дистанция от игрока до ОРИГИНА машины. Не слишком мало —
// у длинных машин оригин в середине, а чинят от капота.
constexpr float REPAIR_RADIUS = 5.0f;

// Справка витрины. Пункты «не сработает» — те же проверки, что в repair/finishRepair.
const char *const TOOLKIT_SHOP_DESCRIPTION =
    "Набор инструментов чинит машину прямо на дороге.\n\n"
    "Как пользоваться: встаньте рядом с машиной и введите /repair.\n"
    "Чинить нужно пешком, машина должна быть не дальше 5 метров.\n"
    "Ремонт занимает 5 секунд, всё это время идёт анимация.\n"
    "Машина чинится ПОЛНОСТЬЮ, тратится один набор.\n"
    "С собой можно носить не больше 3 наборов.\n\n"
    "Подводные камни:\n"
    "- целую машину чинить нельзя — набор не потратится впустую;\n"
    "- ремонт НЕ замораживает вас: если уйти или сесть за руль,\n"
    "  до того как пройдут 5 секунд, ремонт сорвётся;\n"
    "- набор списывается по завершении, а не в начале.";

// «Смотрит на машину»: половина угла конуса. cos(45°) — машина должна быть в
// передней полусфере игрока, а не сбоку и не за спиной.
constexpr float LOOK_MIN_COS = 0.7071f;

// Сколько длится починка. Всё это время игрок свободен в движении — отойдёт, и
// проверка на финише просто не даст починить (замораживать управление нельзя).
constexpr Milliseconds REPAIR_DURATION{5000};

// Анимация возни с машиной: присел и работает руками. Библиотеку обязательно
// предзагружать — первый ApplyAnimation из незагруженной либы не проигрывается.
const char *const REPAIR_ANIM_LIB = "BOMBER";
const char *const REPAIR_ANIM_NAME = "BOM_Plant_Loop";

// Допуск «машина целая»: HP float, ровно 1000 бывает не всегда.
constexpr float FULL_HEALTH_EPS = 1.0f;
} // namespace

ToolkitSystem::ToolkitSystem(ICore &core, const ServiceRegister &serviceRegister)
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

    // Регистрация типа предмета в базовой системе вещей — как у аптечки. Выдача
    // дев-меню (/idev) подхватит новый тип сама, отдельной команды не нужно.
    m_inventory.registerItem(ITEM_TOOLKIT, "Инструменты", TOOLKIT_MAX);

    static_assert(TOOLKIT_MAX == 3, "справка в shopDescription называет размер стека — обнови текст");

    // Либа анимации грузится заранее: первая починка иначе прошла бы без анимации.
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            m_animationService.preloadLibrary(player, REPAIR_ANIM_LIB);
        });

    // Конец сессии: снять флаг и таймер — переиспользованный слот не должен
    // унаследовать чужую починку.
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            const int playerId = player.getID();
            if (!validPlayerId(playerId))
            {
                return;
            }
            m_timers.cancel(m_pendingTimer[playerId]);
            m_repairing[playerId] = false;
            m_targetVehicle[playerId] = -1;
        });

    serviceRegister.getService<PlayerCommandService>().add(
        "repair", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            repair(player);
        },
        {}, "починить машину инструментами (стоя у неё)", PlayerCommandService::HelpCategory::Misc);
}

IVehicle *ToolkitSystem::targetVehicle(int playerId, std::string &reason) const
{
    const Vector3 position = m_locationService.getPosition(playerId);

    GridService::Result nearest;
    if (!m_gridService.closest(position, REPAIR_RADIUS, gridMask(GridEntityType::Vehicle), nearest))
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
    // Клиентский yaw — единственный источник факта «куда смотрит», зато сама
    // ПОЗИЦИЯ обоих серверная, поэтому «дотянуться» через карту нельзя.
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

int ToolkitSystem::repairerOf(int vehicleId, int exceptPlayerId) const
{
    if (vehicleId < 0)
    {
        return -1;
    }
    for (int p = 0; p < MAX_PLAYERS; ++p)
    {
        if (p != exceptPlayerId && m_repairing[p] && m_targetVehicle[p] == vehicleId)
        {
            return p;
        }
    }
    return -1;
}

const char *ToolkitSystem::shopDescription()
{
    return TOOLKIT_SHOP_DESCRIPTION;
}

void ToolkitSystem::repair(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId))
    {
        return;
    }

    if (m_repairing[playerId])
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы уже чините машину"));
        return;
    }
    // Чинят снаружи: из салона до мотора не дотянуться. Стейт серверный.
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из машины, чтобы её починить"));
        return;
    }

    std::string reason;
    IVehicle *vehicle = targetVehicle(playerId, reason);
    if (!vehicle)
    {
        player.sendClientMessage(ERROR_COLOUR, u(reason));
        return;
    }

    // Машину уже чинят: пускать второго нельзя — оба списали бы по инструменту за
    // одну и ту же починку, а второй ремонт пришёлся бы на уже целую машину.
    const int busy = repairerOf(vehicle->getID(), playerId);
    if (busy >= 0)
    {
        IPlayer *other = m_core.getPlayers().get(busy);
        const std::string name =
            other ? Encoding::neutralizeColorCodes(
                        std::string_view(other->getName().data(), other->getName().size()))
                  : std::string("другой игрок");
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Эту машину уже чинит {}[{}]", name, busy)));
        return;
    }

    // Целую машину не чиним — иначе инструмент тратился бы впустую (тот же порядок
    // проверок, что у аптечки: сперва «не нужно», потом «нечем»).
    if (m_vehicleService.getHealth(vehicle->getID()) >= VehicleService::MAX_HEALTH - FULL_HEALTH_EPS)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина цела — чинить нечего"));
        return;
    }
    if (m_inventory.count(playerId, ITEM_TOOLKIT) == 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас нет инструментов"));
        return;
    }

    m_repairing[playerId] = true;
    m_targetVehicle[playerId] = vehicle->getID();

    // Прерываемая анимация: игрок волен уйти, сервер его не держит (замораживать
    // управление в проекте запрещено). Уход поймает проверка на финише.
    m_animationService.play(player, AnimationData(4.1f, true, false, false, false, 0, REPAIR_ANIM_LIB,
                                                  REPAIR_ANIM_NAME),
                            true);
    player.sendClientMessage(INFO_COLOUR, u("Вы принялись за ремонт..."));

    m_pendingTimer[playerId] = m_timers.setPlayerTimeout(player, REPAIR_DURATION,
                                                         [this](IPlayer &p)
                                                         {
                                                             finishRepair(p);
                                                         });
}

void ToolkitSystem::finishRepair(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId) || !m_repairing[playerId])
    {
        return;
    }
    const int targetId = m_targetVehicle[playerId];
    m_repairing[playerId] = false;
    m_targetVehicle[playerId] = -1;
    m_animationService.stop(player);

    // Ре-валидация: пять секунд — достаточный срок, чтобы отойти, сесть в машину или
    // остаться без инструментов. Ничего из этого не должно чинить машину.
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Ремонт прерван — вы сели в машину"));
        return;
    }

    std::string reason;
    IVehicle *vehicle = targetVehicle(playerId, reason);
    if (!vehicle || vehicle->getID() != targetId)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Ремонт прерван — вы отошли от машины"));
        return;
    }
    if (m_inventory.count(playerId, ITEM_TOOLKIT) == 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас нет инструментов"));
        return;
    }

    // Чиним и только ПОТОМ списываем инструмент — как аптечка списывается по факту
    // восстановленного HP.
    m_vehicleService.repair(*vehicle);
    m_inventory.remove(playerId, ITEM_TOOLKIT, 1);

    const int left = m_inventory.count(playerId, ITEM_TOOLKIT);
    player.sendClientMessage(
        INFO_COLOUR, u(fmt::format("Машина починена. Осталось инструментов: {}", left)));
}
