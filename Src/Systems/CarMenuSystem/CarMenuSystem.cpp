#include "Systems/CarMenuSystem/CarMenuSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
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
    if (mode == FamilyService::NO_FAMILY)
        return "у дома";
    if (mode != -1)
        return "в семье";
    return liveVehicleId == -1 ? "в гараже" : "вызвана";
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
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    auto &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add("car", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showCarList(player); }, {},
                 "меню личного транспорта: припарковать у дома, расшарить семье, на карте",
                 PlayerCommandService::HelpCategory::Misc);
}

void CarMenuSystem::showCarList(IPlayer &player)
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

    // LIST: «{n}. Модель {model}  —  {статус}» — формат байт-в-байт с парковкой.
    // Статус: «у дома»/«в семье»/«в гараже»/«вызвана» (единый словарь placementStatus).
    // Имя машины SA не показываем — «Модель {id}».
    std::string body;
    for (std::size_t i = 0; i < owned.size(); ++i)
    {
        const PersonalVehicleService::OwnedVehicle &entry = owned[i];
        // Live id: у припаркованной владение detached (vehicleId=-1) — берём из
        // parked-записи, иначе «вызвана/в гараже» считались бы по -1.
        const char *status =
            placementStatus(m_parkedService, entry.dbId, liveVehicleId(entry.vehicleId, entry.dbId));
        body += fmt::format("{}. Модель {}  —  {}\n", i + 1, entry.model, status);
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Личный транспорт", body, "Выбрать", "Закрыть"),
        [this, playerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return; // игрок вышел / закрыл — ничего не делаем
            }
            // Владение могло измениться, пока диалог открыт — ре-получаем owned и
            // отсекаем выход за границы (выбор ведёт в под-меню, а не спавнит).
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
    return {Action::ParkHere, Action::Unpark, Action::ShareToFamily, Action::ShowOnMap};
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

    const std::vector<Action> actions = buildActions(playerId, carIndex);
    std::string body;
    for (const Action action : actions)
    {
        switch (action)
        {
        case Action::ParkHere:
            body += "Припарковать эту машину здесь\n";
            break;
        case Action::Unpark:
            body += "Убрать с парковки\n";
            break;
        case Action::ShareToFamily:
            body += "Расшарить семье\n";
            break;
        case Action::ShowOnMap:
            body += "Показать на карте\n";
            break;
        }
    }

    // Под-диалог LIST. Заголовок «Модель {model}» — по какой машине действия. Правая
    // кнопка «Назад»: под-диалог не корень, у него есть родитель (список /car).
    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Модель {}", model), body, "Выбрать", "Назад"),
        [this, playerId, carIndex, actions](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showCarList(*player); // «Назад» — вернуть в список машин
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
            case Action::ParkHere:
                parkHere(*player, carIndex);
                break;
            case Action::Unpark:
                unpark(*player, carIndex);
                break;
            case Action::ShareToFamily:
                shareToFamily(*player, carIndex);
                break;
            case Action::ShowOnMap:
                showOnMap(*player, carIndex);
                break;
            }
        });
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
    const float angle = veh->getZAngle();

    // Парковка = ре-тег НА МЕСТЕ (тот же liveVehicleId): без destroy/create. Игрок
    // остаётся за рулём (driver-gate только на входе, под сидящим водителем не
    // срабатывает), HP/позиция/состояние сохраняются. Порядок безопасен: сначала
    // тег/spawn на живом валидном veh, затем запись+setVehicleId, затем detach.
    // a) серверный тег Player -> Parked (ownerId служебный -1: доступ решает canDrive).
    m_vehicleService.setOwner(*veh, VehicleService::Owner::Parked, -1);
    // b) spawn-позиция = текущая точка: death-респавн ядра вернёт машину сюда.
    m_vehicleService.setSpawnPosition(*veh, spot, angle);
    // c) запись парковки (память + write-through INSERT) + связь с живым экземпляром =
    // ТЕМ ЖЕ liveVehicleId (машину не создаём).
    m_parkedService.park(dbId, session->accountId, model, spot, angle);
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
        // машину в мир нечем. Снимаем только запись (владение и так в гараже,
        // vehicleId=-1); на следующем заходе игрок возьмёт машину на центральной парковке.
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
    // Расшарить можно только УЖЕ припаркованную ЛИЧНО машину (шеринг = режим доступа
    // поверх парковки; машину НЕ пересоздаём).
    const int mode = m_parkedService.parkedMode(dbId);
    if (mode == -1)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сначала припаркуйте машину у дома, потом откроете её семье"));
        return;
    }
    if (mode != FamilyService::NO_FAMILY)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина уже расшарена семье"));
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
    const std::string note = u(fmt::format("[{}] Появилась новая семейная машина (модель {})", family->name, model));
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
