#include "Systems/CarMenuSystem/CarMenuSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/VehicleService/VehicleModelNames.h"
#include "Services/ParkedVehicleService/ParkedVehicleCallNotice.h"
#include "Services/ParkedVehicleService/ParkedVehicleRow.h"
#include "Systems/Core/VehicleControlSystem/VehicleEngineNotice.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include "glm/geometric.hpp"
#include <algorithm>
#include <cstddef>
#include <fmt/format.h>
#include <string>

namespace
{
// Игровые цвета сообщений (как в ParkingSystem/HouseSystem/FamilySystem).
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Радиус, в котором можно припарковать машину у СВОЕГО дома (двор дома, не полгорода).
constexpr float PARK_HOUSE_RADIUS = 30.0f;
} // namespace

CarMenuSystem::CarMenuSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_personalService(serviceRegister.getService<PersonalVehicleService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_familyService(serviceRegister.getService<FamilyService>()),
      m_parkedService(serviceRegister.getService<ParkedVehicleService>()),
      m_houseService(serviceRegister.getService<HouseService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_lockService(serviceRegister.getService<VehicleLockService>()),
      m_screenNotice(serviceRegister.getService<ScreenNoticeService>())
{
    auto &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add("car", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showRoot(player); }, {},
                 "меню личного транспорта: текущая машина, парковка, семья, карта",
                 PlayerCommandService::HelpCategory::Misc);
}

// ---------------------------------------------------------------- корень /car

void CarMenuSystem::showRoot(IPlayer &player)
{
    const int playerId = player.getID();
    // Первая строка подсказывает состояние ДО клика: сидит в своей машине — её
    // имя, иначе «не за рулем» (гейт всё равно в обработчике — лейбл лишь превью).
    IVehicle *current = ownCurrentVehicle(playerId);
    const std::string currentLine =
        current ? fmt::format("Текущая машина - {}", VehicleModelNames::displayName(current->getModel()))
                : std::string("Текущая машина - не за рулем");
    Dialog dialog = makeDialog(DialogStyle_LIST, "Личный транспорт", currentLine + "\nМои машины", "Выбрать",
                               "Закрыть");
    m_dialogService.show(
        player, dialog,
        [this, playerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return;
            }
            if (listItem == 0)
            {
                showCurrentVehicle(*player);
            }
            else if (listItem == 1)
            {
                showMyCars(*player);
            }
        });
}

// -------------------------------------------------------- раздел «Текущая машина»

IVehicle *CarMenuSystem::ownCurrentVehicle(int playerId) const
{
    IVehicle *vehicle = m_vehicleService.getVehicle(playerId);
    if (!vehicle)
    {
        return nullptr;
    }
    const int vehicleId = vehicle->getID();

    // Личная сессионная — тег Owner::Player с ownerId == playerId (серверный, не
    // клиентский). Такую водит только владелец по определению тега.
    if (m_vehicleService.getOwner(vehicleId) == VehicleService::Owner::Player &&
        m_vehicleService.getOwnerId(vehicleId) == playerId)
    {
        return vehicle;
    }

    // Припаркованная (у дома/в семье): «своя» — владелец ЗАПИСИ по accountId сессии.
    // Расшаренная семье машина ЧУЖОГО владельца НЕ своя (это гейт «Текущая машина»,
    // не canDrive за руль — сидеть можно и не будучи владельцем, если расшарена).
    if (const ParkedVehicleService::Parked *parked = m_parkedService.byVehicleId(vehicleId))
    {
        const PlayerSessionService::AccountId accountId = m_sessionService.getAccountId(playerId);
        if (accountId != PlayerSessionService::NO_ACCOUNT && accountId == parked->ownerAccountId)
        {
            return vehicle;
        }
    }
    return nullptr;
}

int CarMenuSystem::currentCarIndex(int playerId) const
{
    IVehicle *vehicle = ownCurrentVehicle(playerId);
    if (!vehicle)
    {
        return -1;
    }
    // Своя машина всегда соответствует записи владения: сессионная — по vehicleId
    // владения, припаркованная — через parked-запись (liveVehicleId). Не нашлась —
    // окно регистрации/рассинхрон зеркала, ведём себя как «не в своей машине».
    const int vehicleId = vehicle->getID();
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    for (std::size_t i = 0; i < owned.size(); ++i)
    {
        if (liveVehicleId(owned[i].vehicleId, owned[i].dbId) == vehicleId)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void CarMenuSystem::showCurrentVehicle(IPlayer &player)
{
    const int carIndex = currentCarIndex(player.getID());
    if (carIndex == -1)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы не в своей машине"));
        return;
    }
    showCarMenu(player, carIndex, MenuOrigin::Root);
}

// Живой экземпляр машины carIndex, в котором игрок СИДИТ сейчас (любое сиденье);
// nullptr — машины нет в мире либо игрок не внутри неё. Гейт тумблеров единого
// меню: управлять двигателем/фарами/замком можно только изнутри СВОЕЙ машины
// (владение уже гарантирует carIndex из owned, ре-валидированный вызывающим).
IVehicle *CarMenuSystem::seatedVehicle(int playerId, const PersonalVehicleService::OwnedVehicle &entry) const
{
    const int liveId = liveVehicleId(entry.vehicleId, entry.dbId);
    IVehicle *vehicle = liveId != -1 ? m_vehicleService.get(liveId) : nullptr;
    if (!vehicle || m_vehicleService.getVehicle(playerId) != vehicle)
    {
        return nullptr;
    }
    return vehicle;
}

void CarMenuSystem::toggleEngine(IPlayer &player, int carIndex, MenuOrigin origin)
{
    const int playerId = player.getID();
    // Ре-валидация на КАЖДОМ клике — диалог мог висеть, пока владение/посадка менялись.
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }
    // Разные причины — разные тексты: машины нет в мире (инструкция «сядьте» была
    // бы невыполнимой) vs в мире, но игрок не внутри.
    if (liveVehicleId(owned[carIndex].vehicleId, owned[carIndex].dbId) == -1)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("{}, чтобы управлять", noInstanceHint(owned[carIndex].dbId))));
        showCarMenu(player, carIndex, origin);
        return;
    }
    IVehicle *vehicle = seatedVehicle(playerId, owned[carIndex]);
    if (!vehicle)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сядьте в эту машину, чтобы управлять двигателем"));
        showCarMenu(player, carIndex, origin);
        return;
    }
    const int vehicleId = vehicle->getID();
    const bool on = vehicle->getParams().engine != 0;
    const bool wantsStart = !on;

    // Причины отказа и обратная связь ОДИНАКОВЫ с клавишным тумблером
    // (VehicleControlSystem): тот же попап VehicleEngineNotice (не чат), единый
    // текст/цвет. Заглохшую (stall) чинит только repair(), пустой бак — refuel().
    // Заглушить можно всегда. После попапа переоткрываем меню.
    if (wantsStart && m_vehicleService.isStalled(vehicleId))
    {
        VehicleEngineNotice::showEngineBroken(m_screenNotice, player);
        showCarMenu(player, carIndex, origin);
        return;
    }
    if (wantsStart && m_vehicleService.isOutOfFuel(vehicleId))
    {
        VehicleEngineNotice::showNoFuel(m_screenNotice, player);
        showCarMenu(player, carIndex, origin);
        return;
    }

    m_vehicleService.setEngine(*vehicle, !on);
    player.sendClientMessage(INFO_COLOUR, !on ? u("Двигатель заведён") : u("Двигатель заглушён"));
    showCarMenu(player, carIndex, origin); // переоткрыть с обновлёнными лейблами
}

void CarMenuSystem::toggleLights(IPlayer &player, int carIndex, MenuOrigin origin)
{
    const int playerId = player.getID();
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }
    if (liveVehicleId(owned[carIndex].vehicleId, owned[carIndex].dbId) == -1)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("{}, чтобы управлять", noInstanceHint(owned[carIndex].dbId))));
        showCarMenu(player, carIndex, origin);
        return;
    }
    IVehicle *vehicle = seatedVehicle(playerId, owned[carIndex]);
    if (!vehicle)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сядьте в эту машину, чтобы управлять фарами"));
        showCarMenu(player, carIndex, origin);
        return;
    }
    const bool on = vehicle->getParams().lights == 1;
    m_vehicleService.setLights(*vehicle, !on);
    player.sendClientMessage(INFO_COLOUR, !on ? u("Фары включены") : u("Фары выключены"));
    showCarMenu(player, carIndex, origin);
}

void CarMenuSystem::toggleLock(IPlayer &player, int carIndex, MenuOrigin origin)
{
    const int playerId = player.getID();
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }
    // Замок — «брелок»: в отличие от двигателя/фар посадка НЕ нужна, работает на
    // любом расстоянии (уехал-вышел-забыл закрыть -> закрывается из /car). Гейт
    // владения уже дал carIndex (owned = свои); нужен лишь живой экземпляр —
    // замок сессионный и живёт на экземпляре, «на парковке» запирать нечего.
    const int liveId = liveVehicleId(owned[carIndex].vehicleId, owned[carIndex].dbId);
    if (liveId == -1)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("{}, чтобы управлять", noInstanceHint(owned[carIndex].dbId))));
        showCarMenu(player, carIndex, origin);
        return;
    }
    const bool nowLocked = m_lockService.toggle(liveId);
    player.sendClientMessage(INFO_COLOUR, nowLocked ? u("Двери закрыты") : u("Двери открыты"));
    showCarMenu(player, carIndex, origin);
}

// -------------------------------------------------------- раздел «Мои машины»

void CarMenuSystem::showMyCars(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }

    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (owned.empty())
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u("У вас нет личного транспорта. Купите машину и вызовите её на парковке"));
        return;
    }

    // TABLIST_HEADERS: колонки «Машина | Где находится» (образец —
    // WeaponProficiencySystem::showSkills). Первая строка — шапка колонок, НЕ пункт
    // списка (listItem индексирует строки данных с 0, см. FactionSystem::showMembersMenu).
    // Строка каждой машины — общий builder (единый с /family «Транспорт семьи»).
    std::string body = "Машина\tГде находится\tТопливо\n";
    for (const PersonalVehicleService::OwnedVehicle &entry : owned)
    {
        const int liveId = liveVehicleId(entry.vehicleId, entry.dbId);
        const float fallbackFuel = entry.fuel >= 0.0f ? entry.fuel : VehicleService::FUEL_CAPACITY;
        body += ParkedVehicleRow::build(m_parkedService, m_vehicleService, m_core.getPlayers(), entry.dbId, liveId,
                                        entry.model, fallbackFuel) +
                "\n";
    }
    body.pop_back(); // убрать хвостовой '\n' — иначе пустая строка-фантом в tablist (как AdminSystem::cmdAdminHelp)

    m_dialogService.show(
        player, makeDialog(DialogStyle_TABLIST_HEADERS, "Мои машины", body, "Выбрать", "Назад"),
        [this, playerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showRoot(*player);
                return;
            }
            const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
            if (listItem < 0 || listItem >= static_cast<int>(owned.size()))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
                return;
            }
            showCarMenu(*player, listItem, MenuOrigin::MyCars);
        });
}

std::vector<CarMenuSystem::Action> CarMenuSystem::buildActions(int /*playerId*/, int /*carIndex*/) const
{
    // Правило видимости: все пункты видны ВСЕГДА (по членству/роли/состоянию/посадке
    // не прячем) — недоступность объясняет сообщением сам обработчик. Пункты прав не
    // дают: обработчик авторитетно гейтит владельца, посадку, дом, семью, dbId и
    // состояние парковки. Тумблеры — сверху (самое частое: игрок сидит в машине).
    // Набор сейчас статичен (параметры не используются) — сверка «набор изменился»
    // в обработчике клика недостижима и оставлена как шов под будущий динамический
    // набор (паттерн FamilySystem::showMenu).
    return {Action::Engine,    Action::Lights,   Action::Lock,   Action::Call,
            Action::Respawn,   Action::ShowOnMap, Action::ParkHere, Action::Unpark,
            Action::ShareToFamily};
}

void CarMenuSystem::showCarMenu(IPlayer &player, int carIndex, MenuOrigin origin)
{
    const int playerId = player.getID();
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }
    const int model = owned[carIndex].model;
    const long long dbId = owned[carIndex].dbId;
    // Динамические лейблы: тумблеры — по живому экземпляру (не в мире — базовый
    // лейбл, клик объяснит), 5-й пункт действий — по режиму парковки (не скрытие —
    // недоступность объясняет обработчик сообщением при клике).
    const int liveId = liveVehicleId(owned[carIndex].vehicleId, dbId);
    IVehicle *live = liveId != -1 ? m_vehicleService.get(liveId) : nullptr;
    const bool engineOn = live && live->getParams().engine != 0;
    const bool lightsOn = live && live->getParams().lights == 1;
    const bool locked = live && m_lockService.isLocked(liveId);
    const int mode = dbId != -1 ? m_parkedService.parkedMode(dbId) : ParkedVehicleService::NOT_PARKED;
    const bool shared = mode != ParkedVehicleService::NOT_PARKED && mode != FamilyService::NO_FAMILY;
    // Вызвана ли она СЕЙЧАС этим игроком — только для лейбла.
    const bool called = dbId != -1 && m_parkedService.calledBy(dbId) == m_sessionService.getAccountId(playerId);

    const std::vector<Action> actions = buildActions(playerId, carIndex);
    std::string body;
    for (const Action action : actions)
    {
        switch (action)
        {
        case Action::Engine:
            body += engineOn ? "Заглушить двигатель\n" : "Завести двигатель\n";
            break;
        case Action::Lights:
            body += lightsOn ? "Выключить фары\n" : "Включить фары\n";
            break;
        case Action::Lock:
            body += locked ? "Открыть двери\n" : "Закрыть двери\n";
            break;
        case Action::Call:
            // Лейбл показывает состояние, но прав не даёт: гейт — в обработчике.
            body += called ? "Вызвать заново\n" : "Вызвать машину\n";
            break;
        case Action::Respawn:
            body += "Респавн\n";
            break;
        case Action::ShowOnMap:
            body += "Показать на карте\n";
            break;
        case Action::ParkHere:
            body += "Припарковать эту машину здесь\n";
            break;
        case Action::Unpark:
            body += "Убрать с парковки\n";
            break;
        case Action::ShareToFamily:
            body += shared ? "Вернуть от семьи\n" : "Передать семье\n";
            break;
        }
    }

    // Единое меню машины (LIST). Заголовок — имя машины. «Назад» -> откуда пришли.
    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, VehicleModelNames::displayName(model), body, "Выбрать", "Назад"),
        [this, playerId, carIndex, actions, origin](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                // «Назад» — в точку входа: корень («Текущая машина») либо список.
                if (origin == MenuOrigin::Root)
                {
                    showRoot(*player);
                }
                else
                {
                    showMyCars(*player);
                }
                return;
            }
            // Машину идентифицирует ЗАХВАЧЕННЫЙ carIndex (НЕ listItem — это выбор
            // ДЕЙСТВИЯ). Ре-валидируем carIndex против актуального owned.
            const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
            if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
                return;
            }
            // Сверяем выбранный пункт с актуальным набором (как FamilySystem::showMenu).
            const std::vector<Action> current = buildActions(playerId, carIndex);
            if (listItem < 0 || static_cast<std::size_t>(listItem) >= actions.size())
            {
                return;
            }
            const Action chosen = actions[listItem];
            if (std::find(current.begin(), current.end(), chosen) == current.end())
            {
                showCarMenu(*player, carIndex, origin); // набор изменился — перепоказать
                return;
            }
            switch (chosen)
            {
            case Action::Engine:
                toggleEngine(*player, carIndex, origin);
                break;
            case Action::Lights:
                toggleLights(*player, carIndex, origin);
                break;
            case Action::Lock:
                toggleLock(*player, carIndex, origin);
                break;
            case Action::Call:
                callCar(*player, carIndex);
                break;
            case Action::Respawn:
                respawnAction(*player, carIndex);
                break;
            case Action::ShowOnMap:
                showOnMap(*player, carIndex);
                break;
            case Action::ParkHere:
                parkHere(*player, carIndex);
                break;
            case Action::Unpark:
                unpark(*player, carIndex);
                break;
            case Action::ShareToFamily:
                shareToFamily(*player, carIndex);
                break;
            }
        });
}

void CarMenuSystem::callCar(IPlayer &player, int carIndex)
{
    const int playerId = player.getID();
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }
    const long long dbId = owned[carIndex].dbId;
    if (dbId == -1)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У этой машины нет места парковки"));
        return;
    }

    // Снимок водителя ДО вызова: повторный вызов уже вызванной машины подаёт её на
    // место и отказывает под сидящим водителем — по этому снимку отказ различает
    // «за рулём сам нажавший» и «чужой водитель».
    const ParkedVehicleService::Parked *rec = m_parkedService.byDbId(dbId);
    const int driverId = rec && rec->vehicleId != -1 ? m_vehicleService.getDriver(rec->vehicleId) : -1;
    // accountId серверный (из сессии) — клиенту не верим; сервис им же и гейтит.
    // Текст ответа общий с /family «Транспорт семьи» (ParkedVehicleCall).
    ParkedVehicleCall::reply(player, m_parkedService.call(dbId, m_sessionService.getAccountId(playerId)),
                             INFO_COLOUR, ERROR_COLOUR, driverId);
}

void CarMenuSystem::respawnAction(IPlayer &player, int carIndex)
{
    const int playerId = player.getID();
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }
    const long long dbId = owned[carIndex].dbId;
    const bool parked = dbId != -1 && m_parkedService.isParked(dbId);

    if (!parked)
    {
        // «Вызвана» (сессионная в мире) либо «в гараже» (нет экземпляра).
        const int vehicleId = owned[carIndex].vehicleId;
        if (vehicleId == -1)
        {
            player.sendClientMessage(ERROR_COLOUR, u("Машина уже на парковке"));
            return;
        }
        const int driverId = m_vehicleService.getDriver(vehicleId);
        if (driverId != -1)
        {
            // Разная причина — разный текст: сам за рулём (частый случай) vs чужой
            // водитель, иначе отказ читается как баг («какой водитель, я один»).
            player.sendClientMessage(ERROR_COLOUR,
                                     driverId == playerId
                                         ? u("Вы сами за рулём — выйдите из машины, чтобы её респавнить")
                                         : u("Пока в машине водитель, респавн недоступен"));
            return;
        }
        // Вернуть на парковку: destroy — subscribeDestroyed сам обнулит vehicleId
        // владения (onWorldVehicleDestroyed), запись владения НЕ удаляется.
        m_vehicleService.destroy(vehicleId);
        player.sendClientMessage(INFO_COLOUR,
                                 u("Машина отправлена на парковку. Возьмите её там"));
        return;
    }

    // Припаркована (у дома/в семье): вернуть на точку через respawnHome (снимок/
    // восстановление бака уже внутри ParkedVehicleService/ParkedVehicleSystem —
    // никакого бесплатного топлива).
    const ParkedVehicleService::Result result = m_parkedService.respawnHome(dbId);
    switch (result)
    {
    case ParkedVehicleService::Result::Ok:
        player.sendClientMessage(INFO_COLOUR, u("Машина возвращена на свою точку у дома"));
        break;
    case ParkedVehicleService::Result::Occupied:
    {
        // Тот же развод текстов, что у «вызванной»: сам за рулём vs чужой водитель.
        const ParkedVehicleService::Parked *rec = m_parkedService.byDbId(dbId);
        const int driverId =
            rec && rec->vehicleId != -1 ? m_vehicleService.getDriver(rec->vehicleId) : -1;
        player.sendClientMessage(ERROR_COLOUR,
                                 driverId == playerId
                                     ? u("Вы сами за рулём — выйдите из машины, чтобы её респавнить")
                                     : u("Пока в машине водитель, респавн недоступен"));
        break;
    }
    case ParkedVehicleService::Result::NoInstance:
        // Штатное состояние припаркованной: её не вызывали, в мире машины нет —
        // возвращать на точку нечего, вызов и есть «подать машину на место».
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина в гараже — вызовите её, и она приедет на своё место"));
        break;
    default:
        player.sendClientMessage(ERROR_COLOUR, u("Сейчас нельзя вернуть машину, попробуйте позже"));
        break;
    }
}

void CarMenuSystem::parkHere(IPlayer &player, int carIndex)
{
    const int playerId = player.getID();

    // accountId серверный (из сессии); нужен для владельца парковки и ключа дома.
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы ещё не авторизованы"));
        return;
    }

    // Своя машина по playerId (владение серверное); ре-валидируем индекс/dbId.
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }
    const long long dbId = owned[carIndex].dbId;
    const int model = owned[carIndex].model;
    const int liveVehicleId = owned[carIndex].vehicleId;
    if (dbId == -1)
    {
        // id ещё не присвоен (окно между покупкой и приходом LAST_INSERT_ID).
        player.sendClientMessage(ERROR_COLOUR, u("Машина ещё регистрируется, попробуйте позже"));
        return;
    }
    if (m_parkedService.isParked(dbId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина уже припаркована у дома"));
        return;
    }

    // Сидит ЗА РУЛЁМ именно этой машины: getVehicle == её живой экземпляр И seat==0.
    // Это гарантирует, что машина заспавнена (liveVehicleId != -1) и её позиция известна.
    IVehicle *veh = liveVehicleId != -1 ? m_vehicleService.get(liveVehicleId) : nullptr;
    if (!veh || m_vehicleService.getVehicle(playerId) != veh || m_vehicleService.getSeat(playerId) != 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сядьте за руль этой машины, чтобы припарковать её"));
        return;
    }

    // Свой дом (houseOf по серверному accountId). Нет дома — мягкий отказ с путём.
    const HouseService::House *house = m_houseService.houseOf(std::to_string(session->accountId));
    if (!house)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u("Чтобы парковать машину у дома, нужен свой дом. Займите свободный дом на карте"));
        return;
    }
    const Vector3 spot = veh->getPosition();
    if (glm::distance(spot, house->entrance) > PARK_HOUSE_RADIUS)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Пригоните машину к своему дому, чтобы припарковать её"));
        return;
    }

    // Кап парковки у дома: число уже припаркованных машин владельца (личные + расшаренные
    // семье — все у ЕГО дома по модели «один дом, радиус 30») не должно достигать
    // house->parkingCap. Текущую машину ещё не парковали (isParked дал false выше — её нет
    // в счёте, двойного счёта нет). Кап (houses.json) и счёт (parkedByAccount) серверные;
    // клиент не влияет. parkingCap уже клампнут (>=1) — приведение к size_t безопасно.
    if (m_parkedService.countParkedByAccount(session->accountId) >= static_cast<std::size_t>(house->parkingCap))
    {
        player.sendClientMessage(
            ERROR_COLOUR,
            u(fmt::format("У вашего дома нет места для ещё одной машины (лимит {}). Уберите одну с парковки, "
                          "чтобы поставить эту",
                          house->parkingCap)));
        return;
    }

    const float angle = veh->getZAngle();
    // Снимок остатка топлива НА МОМЕНТ парковки: живой экземпляр ре-тегается НА
    // МЕСТЕ (не пересоздаётся) — INSERT обязан записать РЕАЛЬНЫЙ остаток, не дефолт
    // БД, иначе «доехал впритык -> припарковал -> убрал -> вызвал» доливал бы бак.
    const float fuel = m_vehicleService.getFuel(liveVehicleId);

    // Парковка = ре-тег НА МЕСТЕ (тот же liveVehicleId): без destroy/create. Игрок
    // остаётся за рулём (driver-gate только на входе, под сидящим водителем не
    // срабатывает), HP/позиция/состояние сохраняются. Порядок безопасен: сначала
    // тег/spawn на живом валидном veh, затем запись+setVehicleId, затем detach.
    // a) серверный тег Player -> Parked (ownerId служебный -1: доступ решает canDrive).
    m_vehicleService.setOwner(*veh, VehicleService::Owner::Parked, -1);
    // b) spawn-позиция = текущая точка: death-респавн ядра вернёт машину сюда.
    m_vehicleService.setSpawnPosition(*veh, spot, angle);
    // c) запись парковки (память + write-through INSERT, с реальным fuel) + связь с
    // живым экземпляром = ТЕМ ЖЕ liveVehicleId (машину не создаём). park помечает
    // запись вызванной владельцем: машина стоит здесь, пока он в игре, а на выходе
    // уходит из мира — у точки парковки никто не ждёт.
    m_parkedService.park(dbId, session->accountId, model, spot, angle, fuel, liveVehicleId);
    // d) КРИТИЧНО: отвязать от сессионного трекинга Personal (vehicleId владения -> -1),
    // иначе reset на дисконнекте владельца уничтожит припаркованную машину. Без destroy.
    m_personalService.detach(dbId);

    player.sendClientMessage(
        INFO_COLOUR,
        u("Машина припаркована у вашего дома. После вашего выхода она уедет в гараж — вызвать её на это место "
          "можно через /car"));
}

void CarMenuSystem::unpark(IPlayer &player, int carIndex)
{
    const int playerId = player.getID();

    // Своя машина по playerId (владение серверное); ре-валидируем индекс/dbId.
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }
    const long long dbId = owned[carIndex].dbId;
    if (dbId == -1 || !m_parkedService.isParked(dbId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина не припаркована у дома"));
        return;
    }

    // Живой экземпляр берём из parked-записи: во владении vehicleId уже -1 (detach на
    // парковке отвязал его от сессионного трекинга).
    const ParkedVehicleService::Parked *parked = m_parkedService.byDbId(dbId);
    const int carId = parked ? parked->vehicleId : -1;
    IVehicle *veh = carId != -1 ? m_vehicleService.get(carId) : nullptr;
    if (!veh)
    {
        // Машины нет в мире — обычное состояние припаркованной, пока её не вызвали
        // (и редкий случай внешнего destroy между показом меню и кликом): ре-тегать
        // на месте нечего. Переносим последний известный снимок fuel записи в
        // владение (иначе следующий спавн через центральную парковку взял бы
        // устаревший entry.fuel, каким он был ДО парковки у дома). Снимаем только
        // запись (владение и так в гараже, vehicleId=-1); на следующем заходе игрок
        // возьмёт машину на центральной парковке.
        if (parked)
        {
            m_personalService.setFuel(playerId, carIndex, parked->fuel);
        }
        m_parkedService.unparkKeepInstance(dbId);
        player.sendClientMessage(INFO_COLOUR, u("Машина убрана с парковки"));
        return;
    }

    // Снятие = ре-тег НА МЕСТЕ обратно в личную сессионную, БЕЗ destroy (тот же carId):
    // машина остаётся стоять где стоит, не исчезает и не телепортируется.
    // a) серверный тег Parked -> Player (снова личная владельца).
    m_vehicleService.setOwner(*veh, VehicleService::Owner::Player, playerId);
    // b) вернуть в сессионный трекинг Personal (обычный жизненный цикл: уничтожение на
    // дисконнекте/смерти, как у обычной личной сессионной).
    m_personalService.attach(dbId, carId);
    // c) снять запись парковки + DELETE, НЕ трогая живой экземпляр (unparkKeepInstance).
    // Если была расшарена семье — доступ семьи уходит вместе со снятием записи.
    m_parkedService.unparkKeepInstance(dbId);
    player.sendClientMessage(INFO_COLOUR, u("Машина снята с парковки"));
}

void CarMenuSystem::shareToFamily(IPlayer &player, int carIndex)
{
    const int playerId = player.getID();

    // Своя машина по playerId (владение серверное); ре-валидируем индекс/dbId.
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }
    const long long dbId = owned[carIndex].dbId;
    const int model = owned[carIndex].model;
    if (dbId == -1)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Машина ещё регистрируется, попробуйте позже"));
        return;
    }
    const int mode = m_parkedService.parkedMode(dbId);

    if (mode != ParkedVehicleService::NOT_PARKED && mode != FamilyService::NO_FAMILY)
    {
        // Уже расшарена -> «Вернуть от семьи»: гейт — владелец МАШИНЫ (машина всегда
        // личная, шарится только своя припаркованная — быть владельцем СЕМЬИ не нужно).
        const FamilyService::Family *family = m_familyService.getFamily(mode);
        const ParkedVehicleService::Result result = m_parkedService.unshareFromFamily(dbId);
        switch (result)
        {
        case ParkedVehicleService::Result::Ok:
        {
            player.sendClientMessage(INFO_COLOUR, u("Машина возвращена от семьи. Теперь ей пользуетесь только вы"));
            // Симметрично shareToFamily: уведомить онлайн-членов семьи.
            if (family)
            {
                const std::string note = u(fmt::format("[{}] Семейная машина ({}) возвращена владельцу",
                                                       family->name, VehicleModelNames::displayName(model)));
                for (IPlayer *member : m_core.getPlayers().entries())
                {
                    if (member->getID() != playerId && m_familyService.getFamilyId(member->getID()) == mode)
                    {
                        member->sendClientMessage(INFO_COLOUR, note);
                    }
                }
            }
            break;
        }
        case ParkedVehicleService::Result::NotShared:
        case ParkedVehicleService::Result::NotParked:
        default:
            player.sendClientMessage(ERROR_COLOUR, u("Сейчас нельзя вернуть машину, попробуйте позже"));
            break;
        }
        return;
    }

    // Не расшарена -> «Передать семье» (прежняя логика shareToFamily, без изменений).
    // Порядок отказов — от самой ранней реальной причины. Сначала членство: без семьи
    // «только владелец» — ложная причина, игроку нужна семья, а не роль (даём путь).
    const int familyId = m_familyService.getFamilyId(playerId);
    if (familyId == FamilyService::NO_FAMILY)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас пока нет семьи. Создайте её через /family"));
        return;
    }
    // Владелец семьи — серверная проверка (isOwner по аккаунту слота, не по клиенту).
    if (!m_familyService.isOwner(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Расшарить машину семье может только лидер семьи"));
        return;
    }
    const FamilyService::Family *family = m_familyService.getFamily(familyId);
    if (!family)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы не состоите в семье"));
        return;
    }
    // Расшарить можно только УЖЕ припаркованную ЛИЧНО машину (шеринг = режим доступа
    // поверх парковки; машину НЕ пересоздаём).
    if (mode == ParkedVehicleService::NOT_PARKED)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сначала припаркуйте машину у дома, потом откроете её семье"));
        return;
    }

    // Флип доступа: только UPDATE family_id, тот же экземпляр Owner::Parked. Замок
    // начнёт пускать членов семьи на СЛЕДУЮЩЕЙ посадке за руль.
    const ParkedVehicleService::Result result = m_parkedService.shareToFamily(dbId, familyId);
    if (result != ParkedVehicleService::Result::Ok)
    {
        // Гонка (машину убрали/расшарили между показом и кликом) — сообщаем и выходим.
        player.sendClientMessage(ERROR_COLOUR, u("Сейчас нельзя расшарить машину, попробуйте позже"));
        return;
    }

    player.sendClientMessage(INFO_COLOUR,
                             u("Машина расшарена семье. Теперь ей могут пользоваться все члены семьи"));

    // Уведомить онлайн-членов семьи о новой машине (как join/leave в /f).
    const std::string note = u(fmt::format("[{}] Появилась новая семейная машина ({})", family->name,
                                           VehicleModelNames::displayName(model)));
    for (IPlayer *member : m_core.getPlayers().entries())
    {
        if (member->getID() != playerId && m_familyService.getFamilyId(member->getID()) == familyId)
        {
            member->sendClientMessage(INFO_COLOUR, note);
        }
    }
}

int CarMenuSystem::liveVehicleId(int ownedVehicleId, long long dbId) const
{
    if (ownedVehicleId != -1)
    {
        return ownedVehicleId; // обычная сессионная — id из владения
    }
    // Припаркованная: владение detached (vehicleId=-1), живой экземпляр — у ParkedService.
    if (dbId != -1)
    {
        if (const ParkedVehicleService::Parked *parked = m_parkedService.byDbId(dbId))
        {
            return parked->vehicleId;
        }
    }
    return -1;
}

const char *CarMenuSystem::noInstanceHint(long long dbId) const
{
    // Припаркованная у дома в мире не ждёт — её надо вызвать; неприпаркованная
    // личная стоит на центральной парковке, её берут там.
    return dbId != -1 && m_parkedService.isParked(dbId) ? "Эта машина в гараже. Вызовите её"
                                                        : "Эта машина на парковке. Возьмите её";
}

void CarMenuSystem::showOnMap(IPlayer &player, int carIndex)
{
    const int playerId = player.getID();
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }

    // Live vehicleId: у припаркованной владение detached (vehicleId=-1), живой экземпляр
    // держит ParkedVehicleService — иначе показали бы «в гараже», хотя машина у дома.
    const int vehicleId = liveVehicleId(owned[carIndex].vehicleId, owned[carIndex].dbId);
    if (vehicleId == -1)
    {
        // Владение есть, но машина не в мире — отметить нечего.
        player.sendClientMessage(
            ERROR_COLOUR,
            u(fmt::format("{}, чтобы отметить на карте", noInstanceHint(owned[carIndex].dbId))));
        return;
    }

    IVehicle *veh = m_vehicleService.get(vehicleId);
    if (!veh)
    {
        // Была вызвана, но исчезла (уничтожена между открытием меню и выбором).
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        return;
    }

    m_waypointService.showFor(player, *veh);
    player.sendClientMessage(INFO_COLOUR, u("Ваша машина отмечена на карте красным чекпоинтом"));
}
