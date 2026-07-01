#include "Systems/CarMenuSystem/CarMenuSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <cstddef>
#include <fmt/format.h>
#include <string>

namespace
{
// Игровые цвета сообщений (как в ParkingSystem/HouseSystem/FamilySystem).
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

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

CarMenuSystem::CarMenuSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_personalService(serviceRegister.getService<PersonalVehicleService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    auto &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add("car", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showCarList(player); }, {},
                 "меню личного транспорта: показать машину на карте", PlayerCommandService::HelpCategory::Misc);
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
    // Статус: «в гараже» (vehicleId==-1) / «вызвана» (vehicleId!=-1) — /car зовут по
    // всей карте, «на парковке» тут ввело бы в заблуждение (машина может стоять где
    // угодно). Имя машины SA не показываем — «Модель {id}».
    std::string body;
    for (std::size_t i = 0; i < owned.size(); ++i)
    {
        const PersonalVehicleService::OwnedVehicle &entry = owned[i];
        const char *status = entry.vehicleId == -1 ? "в гараже" : "вызвана";
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

    // Под-диалог LIST (задел на рост — пунктов будет больше). Пункт 0 «Показать на
    // карте». Заголовок «Модель {model}» — по какой машине действия. Правая кнопка
    // «Назад»: под-диалог не корень, у него есть родитель (список /car).
    const std::string body = "Показать на карте\n";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Модель {}", model), body, "Выбрать", "Назад"),
        [this, playerId, carIndex](DialogResponse response, int listItem, StringView)
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
            // Машину идентифицирует ЗАХВАЧЕННЫЙ carIndex (НЕ listItem под-диалога —
            // это выбор ДЕЙСТВИЯ). Ре-валидируем carIndex против актуального owned.
            const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
            if (carIndex < 0 || carIndex >= static_cast<int>(owned.size()))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
                return;
            }
            // listItem — выбор действия под-диалога. Пока действие одно: 0 = карта.
            if (listItem == 0)
            {
                showOnMap(*player, carIndex);
            }
        });
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

    const int vehicleId = owned[carIndex].vehicleId;
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
