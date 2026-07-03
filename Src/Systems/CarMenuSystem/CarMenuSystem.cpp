#include "Systems/CarMenuSystem/CarMenuSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/VehicleService/VehicleModelNames.h"
#include "Systems/Core/VehicleControlSystem/VehicleEngineNotice.h"
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

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

// Статус машины для списков /car (единый словарь с центральной парковкой):
//  «у дома» — припаркована лично; «в семье» — припаркована и расшарена; иначе —
//  «в гараже» (не в мире) / «вызвана» (в мире через центральную парковку).
const char *placementStatus(const ParkedVehicleService &parked, long long dbId, int liveVehicleId)
{
    const int mode = parked.parkedMode(dbId);
    if (mode == ParkedVehicleService::NOT_PARKED)
        return liveVehicleId == -1 ? "в гараже" : "вызвана";
    return mode == FamilyService::NO_FAMILY ? "у дома" : "в семье";
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
    Dialog dialog =
        makeDialog(DialogStyle_LIST, "Личный транспорт", "Текущая машина\nМои машины", "Выбрать", "Закрыть");
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

void CarMenuSystem::showCurrentVehicle(IPlayer &player)
{
    const int playerId = player.getID();
    IVehicle *vehicle = ownCurrentVehicle(playerId);
    if (!vehicle)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы не в своей машине"));
        return;
    }

    const VehicleParams params = vehicle->getParams();
    // Трактовки — как у клавишных тумблеров (VehicleControlSystem): двигатель
    // -1 (авто)/1 — «работает» (первый клик глушит); фары только явная 1 — «включены».
    const bool engineOn = params.engine != 0;
    const bool lightsOn = params.lights == 1;
    const bool locked = m_lockService.isLocked(vehicle->getID());

    std::string body;
    body += engineOn ? "Заглушить двигатель\n" : "Завести двигатель\n";
    body += lightsOn ? "Выключить фары\n" : "Включить фары\n";
    body += locked ? "Открыть двери\n" : "Закрыть двери\n";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, VehicleModelNames::displayName(vehicle->getModel()), body, "Выбрать", "Назад"),
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
            switch (listItem)
            {
            case 0:
                toggleEngine(*player);
                break;
            case 1:
                toggleLights(*player);
                break;
            case 2:
                toggleLock(*player);
                break;
            default:
                break;
            }
        });
}

void CarMenuSystem::toggleEngine(IPlayer &player)
{
    const int playerId = player.getID();
    // Ре-валидация «своей машины» на КАЖДОМ клике — диалог мог висеть, пока игрок
    // вышел или машина исчезла.
    IVehicle *vehicle = ownCurrentVehicle(playerId);
    if (!vehicle)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы не в своей машине"));
        return;
    }
    const int vehicleId = vehicle->getID();
    const int8_t engine = vehicle->getParams().engine;
    const bool on = engine != 0;
    const bool wantsStart = !on;

    // Причины отказа и обратная связь ОДИНАКОВЫ с клавишным тумблером
    // (VehicleControlSystem): тот же попап VehicleEngineNotice (не чат), единый
    // текст/цвет. Заглохшую (stall) чинит только repair(), пустой бак — refuel().
    // Заглушить можно всегда. После попапа переоткрываем меню.
    if (wantsStart && m_vehicleService.isStalled(vehicleId))
    {
        VehicleEngineNotice::showEngineBroken(m_screenNotice, player);
        showCurrentVehicle(player);
        return;
    }
    if (wantsStart && m_vehicleService.isOutOfFuel(vehicleId))
    {
        VehicleEngineNotice::showNoFuel(m_screenNotice, player);
        showCurrentVehicle(player);
        return;
    }

    m_vehicleService.setEngine(*vehicle, !on);
    player.sendClientMessage(INFO_COLOUR, !on ? u("Двигатель заведён") : u("Двигатель заглушён"));
    showCurrentVehicle(player); // переоткрыть с обновлёнными лейблами
}

void CarMenuSystem::toggleLights(IPlayer &player)
{
    const int playerId = player.getID();
    IVehicle *vehicle = ownCurrentVehicle(playerId);
    if (!vehicle)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы не в своей машине"));
        return;
    }
    const bool on = vehicle->getParams().lights == 1;
    m_vehicleService.setLights(*vehicle, !on);
    player.sendClientMessage(INFO_COLOUR, !on ? u("Фары включены") : u("Фары выключены"));
    showCurrentVehicle(player);
}

void CarMenuSystem::toggleLock(IPlayer &player)
{
    const int playerId = player.getID();
    IVehicle *vehicle = ownCurrentVehicle(playerId);
    if (!vehicle)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы не в своей машине"));
        return;
    }
    const bool nowLocked = m_lockService.toggle(vehicle->getID());
    player.sendClientMessage(INFO_COLOUR, nowLocked ? u("Двери закрыты") : u("Двери открыты"));
    showCurrentVehicle(player);
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
    std::string body = "Машина\tГде находится\n";
    for (const PersonalVehicleService::OwnedVehicle &entry : owned)
    {
        const char *status = placementStatus(m_parkedService, entry.dbId, liveVehicleId(entry.vehicleId, entry.dbId));
        body += fmt::format("{}\t{}\n", VehicleModelNames::displayName(entry.model), status);
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
            showCarActions(*player, listItem);
        });
}

std::vector<CarMenuSystem::Action> CarMenuSystem::buildActions(int /*playerId*/, int /*carIndex*/) const
{
    // Правило видимости: все пункты видны ВСЕГДА (по членству/роли/состоянию не прячем)
    // — недоступность объясняет сообщением сам обработчик действия. Пункты прав не
    // дают: обработчик авторитетно гейтит владельца, за-рулём, дом, семью, dbId и
    // состояние парковки.
    return {Action::Respawn, Action::ShowOnMap, Action::ParkHere, Action::Unpark, Action::ShareToFamily};
}

void CarMenuSystem::showCarActions(IPlayer &player, int carIndex)
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
    // Лейбл 5-го пункта динамический по текущему режиму парковки (не скрытие —
    // недоступность объясняет обработчик сообщением при клике).
    const int mode = dbId != -1 ? m_parkedService.parkedMode(dbId) : ParkedVehicleService::NOT_PARKED;
    const bool shared = mode != ParkedVehicleService::NOT_PARKED && mode != FamilyService::NO_FAMILY;

    const std::vector<Action> actions = buildActions(playerId, carIndex);
    std::string body;
    for (const Action action : actions)
    {
        switch (action)
        {
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

    // Под-диалог LIST. Заголовок — имя машины. Правая кнопка «Назад» -> «Мои машины».
    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, VehicleModelNames::displayName(model), body, "Выбрать", "Назад"),
        [this, playerId, carIndex, actions](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showMyCars(*player); // «Назад» — вернуть в список машин
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
                showCarActions(*player, carIndex); // набор изменился — перепоказать
                return;
            }
            switch (chosen)
            {
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
            player.sendClientMessage(ERROR_COLOUR, u("Машина уже в гараже"));
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
        // Убрать в гараж: destroy — subscribeDestroyed сам обнулит vehicleId владения
        // (onWorldVehicleDestroyed), запись владения НЕ удаляется.
        m_vehicleService.destroy(vehicleId);
        player.sendClientMessage(INFO_COLOUR,
                                 u("Машина отправлена в гараж. Возьмите её на парковке"));
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
        player.sendClientMessage(ERROR_COLOUR, u("Машина сейчас недоступна"));
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
    // живым экземпляром = ТЕМ ЖЕ liveVehicleId (машину не создаём).
    m_parkedService.park(dbId, session->accountId, model, spot, angle, fuel);
    m_parkedService.setVehicleId(dbId, liveVehicleId);
    // d) КРИТИЧНО: отвязать от сессионного трекинга Personal (vehicleId владения -> -1),
    // иначе reset на дисконнекте владельца уничтожит припаркованную машину. Без destroy.
    m_personalService.detach(dbId);

    player.sendClientMessage(INFO_COLOUR,
                             u("Машина припаркована у вашего дома. Она останется здесь и будет ждать вас"));
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
        // Экземпляр пропал (внешний destroy между показом меню и кликом) — вернуть
        // машину в мир нечем. Переносим последний известный снимок fuel записи в
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
        player.sendClientMessage(ERROR_COLOUR, u("Расшарить машину семье может только владелец семьи"));
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
        player.sendClientMessage(ERROR_COLOUR,
                                 u("Эта машина в гараже. Возьмите её на парковке, чтобы отметить на карте"));
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
