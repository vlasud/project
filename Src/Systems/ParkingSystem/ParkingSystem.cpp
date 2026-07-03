#include "Systems/ParkingSystem/ParkingSystem.h"

#include "Services/Core/VehicleService/VehicleModelNames.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/Geometry/Geometry.h"
#include <fmt/format.h>
#include <string>

namespace
{
// Игровые цвета сообщений (как в HouseSystem/PersonalVehicleSystem).
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Горизонтальный радиус «занятости» точки: машина физически в этом радиусе по XY
// (Z игнорируется — машина оседает на грунт) -> точка занята. 3.0 надёжно ловит
// машину у точки по её длине (~4.5 м); соседние точки в ~4.45 м, origin соседа за
// радиусом — ложного занятия соседней точки нет.
constexpr float SPOT_OCCUPIED_RADIUS = 3.0f;

// Сдвиг проб «вперёд»/«назад» ВДОЛЬ ориентации места (по spot.angle) от центра.
// Место — прямоугольник (машина ~4.5 м длиной), одна круговая проба от центра
// упустила бы машину у переднего/заднего края; три пробы (центр ±offset) покрывают
// длину: центр ловит Y±3.0, вперёд/назад дотягивают до ±5.0. 2.0 < 4.45 (шаг ряда
// по X): для angle 0 «вперёд» — по Y (перпендикулярно ряду), дистанция от вперёд/
// назад-пробы до origin соседа = √(4.45²+2.0²)=4.88 > радиуса 3.0 — соседнее место
// ложно не занимается.
constexpr float SPOT_PROBE_OFFSET = 2.0f;

// Точка пикапа парковки (серверная, фикс.). Исходный facing 182.2892 — ориентир, не
// нужен (пикапу угол не задаётся).
const Vector3 PARKING_PICKUP_POS{1626.2488f, -1136.7443f, 23.9063f};

// Пикап парковки: info-икона «i» (сервис-точка, не машина-икона). Тип 1 — подбор
// по касанию, всегда виден (как пикапы домов/баз).
constexpr int PARKING_PICKUP_MODEL = 1239;
constexpr PickupType PARKING_PICKUP_TYPE = 1;

// 3D-текст над пикапом — игроцкий ориентир (виден всем у точки). Цвет INFO; близкая
// дистанция отрисовки; testLOS=false — виден вплотную без мигания.
const Colour PARKING_LABEL_COLOUR{120, 220, 255};
constexpr float PARKING_LABEL_DRAW_DISTANCE = 25.0f;
constexpr bool PARKING_LABEL_TEST_LOS = false;

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

ParkingSystem::ParkingSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_personalService(serviceRegister.getService<PersonalVehicleService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_parkedService(serviceRegister.getService<ParkedVehicleService>()),
      m_pickupService(serviceRegister.getService<PickupService>()),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_labelService(serviceRegister.getService<TextLabelService>())
{
    // Красный чекпоинт-указатель у поданной машины ведёт общий VehicleWaypointService
    // (его привод VehicleWaypointSystem снимает указатель на уничтожении машины-цели
    // и на дисконнекте). Парковка лишь ставит указатель в момент подачи.
}

void ParkingSystem::initialize(IComponentList * /*components*/)
{
    // Пикап парковки в основном мире (vw 0). К моменту initialize PickupSystem уже
    // получил компонент пикапов (порядок реестра систем).
    m_parkingPickup = m_pickupService.add(PARKING_PICKUP_MODEL, PARKING_PICKUP_TYPE, PARKING_PICKUP_POS,
                                          [this](IPlayer &player) { onParkingPickup(player); }, 0);

    // 3D-текст над пикапом: слово «Парковка» + подпись (utf-8) -> весь текст через
    // u() в cp1251. Цветовой код {B4DCFF} интерпретирует клиент (не наш текст).
    m_parkingLabel = m_labelService.add(u("Парковка\n{B4DCFF}Возьмите свой транспорт"), PARKING_PICKUP_POS,
                                        PARKING_LABEL_COLOUR, PARKING_LABEL_DRAW_DISTANCE, PARKING_LABEL_TEST_LOS);
}

void ParkingSystem::onParkingPickup(IPlayer &player)
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
                                 u("У вас нет личного транспорта. Купите машину, чтобы взять её здесь"));
        return;
    }

    // Диалог LIST: пункт — «{n}. {имя}  —  {статус}» (статус: «у дома»
    // (припаркована лично), «в семье» (расшарена), «на парковке» (не в мире), «вызвана»
    // (в мире) — единый словарь с /car по parkedMode). Имя машины — из каталога
    // VehicleModelNames (displayName: пустое имя -> фолбэк «Модель {id}»).
    std::string body;
    for (std::size_t i = 0; i < owned.size(); ++i)
    {
        const PersonalVehicleService::OwnedVehicle &entry = owned[i];
        const int mode = m_parkedService.parkedMode(entry.dbId);
        const char *status = mode == ParkedVehicleService::NOT_PARKED
                                 ? (entry.vehicleId == -1 ? "на парковке" : "вызвана")
                                 : (mode == FamilyService::NO_FAMILY ? "у дома" : "в семье");
        body += fmt::format("{}. {}  —  {}\n", i + 1, VehicleModelNames::displayName(entry.model), status);
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Личный транспорт", body, "Взять", "Закрыть"),
        [this, playerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return; // игрок вышел / закрыл — ничего не делаем
            }
            // Перепроверяем владение на момент выбора (могло измениться): индекс
            // валидирует spawn(), но отсекаем явный выход за границы заранее.
            const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
            if (listItem < 0 || listItem >= static_cast<int>(owned.size()))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
                return;
            }
            // Припаркованную у дома машину (личную ИЛИ расшаренную) центральная
            // парковка НЕ спавнит: она статична у дома (экземпляр Owner::Parked).
            // Личный спавн вернул бы дубль.
            if (m_parkedService.isParked(owned[listItem].dbId))
            {
                player->sendClientMessage(ERROR_COLOUR, u("Эта машина припаркована у дома"));
                return;
            }
            spawnAtParking(*player, listItem);
        });
}

void ParkingSystem::spawnAtParking(IPlayer &player, int ownedIndex)
{
    const int playerId = player.getID();

    // 5 серверных точек спавна (угол 0). Координаты — от геймдизайна.
    // ИНВАРИАНТ: все точки angle 0 -> пробы isSpotFree идут ⊥ ряду (по Y), соседи
    // отстоят по X (~4.45 м) -> ложного занятия соседа нет (разбор в SPOT_PROBE_OFFSET).
    // Если точку ПОВЕРНУТЬ (angle != 0), пробы уйдут под углом и этот геометрический
    // разбор перестанет держаться — тогда перепроверить занятость соседних точек.
    static const ParkingSystem::SpawnSpot SPAWN_SPOTS[] = {
        {{1648.4778f, -1135.5242f, 24.5531f}, 0.0f}, {{1652.9323f, -1135.9354f, 24.5531f}, 0.0f},
        {{1657.3933f, -1135.8505f, 24.5531f}, 0.0f}, {{1661.7965f, -1135.9092f, 24.5531f}, 0.0f},
        {{1666.2893f, -1136.1720f, 23.8031f}, 0.0f},
    };
    constexpr int SPOT_COUNT = static_cast<int>(sizeof(SPAWN_SPOTS) / sizeof(SPAWN_SPOTS[0]));

    // Свою машину (при пере-спавне) исключаем из проверки занятости — можно встать
    // на её же точку. currentVehicle вернёт -1, если экземпляр не заспавнен.
    const int ownVehicleId = m_personalService.currentVehicle(playerId, ownedIndex);

    int chosenSpot = -1;
    for (int i = 0; i < SPOT_COUNT; ++i)
    {
        if (isSpotFree(SPAWN_SPOTS[i], ownVehicleId))
        {
            chosenSpot = i;
            break;
        }
    }
    if (chosenSpot < 0)
    {
        // Все точки заняты — существующие машины НЕ трогаем.
        player.sendClientMessage(ERROR_COLOUR, u("Нет свободных мест на парковке. Попробуйте позже"));
        return;
    }

    const SpawnSpot &spot = SPAWN_SPOTS[chosenSpot];

    IVehicle *veh = nullptr;
    const PersonalVehicleService::SpawnResult result =
        m_personalService.spawn(playerId, ownedIndex, spot.position, spot.angle, -1, -1, &veh);

    switch (result)
    {
    case PersonalVehicleService::SpawnResult::Ok:
    {
        // Красный чекпоинт-указатель у машины через общий VehicleWaypointService:
        // пере-вызов заменяет прежний указатель, снятие (вход/уничтожение/дисконнект)
        // ведёт его привод (VehicleWaypointSystem). veh валиден на ветке Ok.
        if (veh)
        {
            m_waypointService.showFor(player, *veh);
        }
        player.sendClientMessage(INFO_COLOUR, u("Ваша машина отмечена на карте красным чекпоинтом"));
        break;
    }
    case PersonalVehicleService::SpawnResult::BadIndex:
        player.sendClientMessage(ERROR_COLOUR, u("Эта машина больше недоступна"));
        break;
    case PersonalVehicleService::SpawnResult::PoolFull:
    case PersonalVehicleService::SpawnResult::Unavailable:
        player.sendClientMessage(ERROR_COLOUR, u("Сейчас нельзя подать машину, попробуйте позже"));
        break;
    }
}

bool ParkingSystem::isSpotFree(const SpawnSpot &spot, int excludeVehicleId) const
{
    // Пробы «вперёд»/«назад» вдоль ориентации места по общему хелперу Geometry (та
    // же SA-MP конвенция). Пробы в 2D (XY), Z как у центра — занятость горизонтальная
    // (anyVehicleNear игнорирует Z), хелпер и держит z пробы равной z центра.
    const Vector3 &c = spot.position;
    const Vector3 forward = Geometry::forwardOf(c, spot.angle, SPOT_PROBE_OFFSET);
    const Vector3 back = Geometry::backOf(c, spot.angle, SPOT_PROBE_OFFSET);

    // Свободно, только если ни одна проба не нашла машину. Свою (excludeVehicleId)
    // исключаем во всех трёх — пере-спавн на свою же точку не считает её занятой.
    return !m_vehicleService.anyVehicleNear(c, SPOT_OCCUPIED_RADIUS, excludeVehicleId) &&
           !m_vehicleService.anyVehicleNear(forward, SPOT_OCCUPIED_RADIUS, excludeVehicleId) &&
           !m_vehicleService.anyVehicleNear(back, SPOT_OCCUPIED_RADIUS, excludeVehicleId);
}
