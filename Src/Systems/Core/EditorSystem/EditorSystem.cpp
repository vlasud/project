#include "Systems/Core/EditorSystem/EditorSystem.h"

#include "Services/AdminService/AdminService.h"
#include "ThreadPool/ThreadPool.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/FileNameSanitizer.h"
#include "anim.hpp"
#include "component.hpp"
#include "glm/geometric.hpp"
#include <algorithm>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace
{
const std::string MAPS_DIR = "maps";

// Биты PlayerKeyData::keys (классические значения SA).
constexpr uint32_t KEY_CROUCH = 2;
constexpr uint32_t KEY_SPRINT = 8;
constexpr uint32_t KEY_JUMP = 32;
constexpr uint32_t KEY_WALK = 1024;

constexpr float SPRINT_MULTIPLIER = 5.0f; // крупный шаг / быстрый полёт
constexpr float WALK_MULTIPLIER = 0.2f;   // точный шаг
constexpr float ROTATE_DEG_PER_STEP = 20.0f; // градусов вращения на метр шага клавиш

constexpr float CAMERA_SPEED_MIN = 0.1f;
constexpr float CAMERA_SPEED_MAX = 10.0f;
constexpr float KEY_STEP_MIN = 0.01f;
constexpr float KEY_STEP_MAX = 10.0f;

constexpr int MAX_OBJECT_MODEL = 19999;
constexpr int MAX_ACTOR_SKIN = 311;
constexpr int MIN_VEHICLE_MODEL = 400;
constexpr int MAX_VEHICLE_MODEL = 611;
constexpr int MAX_VEHICLE_COLOUR = 255;
constexpr int MAX_PICKUP_TYPE = 23;   // клиентские типы поведения SA
constexpr int DEFAULT_PICKUP_TYPE = 1; // статичный, не исчезает при касании
constexpr float DEFAULT_CHECKPOINT_RADIUS = 3.0f;

// Зонд FindZ читает позицию стоящего игрока, а это его туловище (~центр педа),
// не ноги: прочитанный Z = уровень земли + PED_ORIGIN_HEIGHT. Объект ставим на
// саму землю, актора — на ту же высоту туловища (его origin такой же), машину —
// чуть выше земли, дальше её усадит клиентская физика.
constexpr float PED_ORIGIN_HEIGHT = 1.0f;     // от ног стоящего педа до его origin
constexpr float VEHICLE_GROUND_OFFSET = 0.5f; // полколеса над землёй
constexpr float PICKUP_GROUND_OFFSET = 0.5f;  // пикап чуть над землёй, чтобы не утонул в текстуре
constexpr float CHECKPOINT_GROUND_OFFSET = 1.0f; // цилиндр чекпоинта рисуется от его Z
// Превью чекпоинта обновляется пересозданием на клиенте — 30 раз в секунду он
// не переваривает, обновляем с троттлингом (финальное значение доедет в меню).
constexpr int CHECKPOINT_REFRESH_TICKS = 5;

// Высота, с которой клиент ищет землю (выше самой высокой точки карты ~ г. Чилиад).
constexpr float GROUND_PROBE_Z = 1500.0f;
constexpr int GROUND_PROBE_WAIT = 10;    // тиков ожидания ответа клиента перед чтением Z
constexpr float PROBE_PARK_Z = -1000.0f; // куда временно прячем сущность, чтобы FindZ не попал в неё

constexpr int MAX_INTERIOR_ID = 255;

// Модель прокси-объекта мыши для актора/машины/чекпоинта (стрелка).
constexpr int MOUSE_PROXY_MARKER_MODEL = 1318;

// Известные интерьеры (https://pawnokit.ru/ru/interiors_id, полный список):
// телепорт сразу выставляет нужный interior id и переносит тело с камерой.
struct InteriorSpot
{
    const char *name;
    int interior;
    float x, y, z;
};
constexpr InteriorSpot INTERIOR_SPOTS[] = {
    {"24/7 1", 17, -25.72f, -187.82f, 1003.54f},
    {"24/7 2", 10, 6.08f, -28.89f, 1003.54f},
    {"24/7 3", 18, -30.98f, -89.68f, 1003.54f},
    {"24/7 4", 16, -26.18f, -140.91f, 1003.54f},
    {"24/7 5", 4, -27.84f, -26.67f, 1003.55f},
    {"24/7 6", 6, -26.83f, -55.58f, 1003.54f},
    {"Loco Low Co.", 2, 611.35f, -77.55f, 997.99f},
    {"Wheel Arch Angels", 3, 612.21f, -123.90f, 997.99f},
    {"TransFender", 1, 621.45f, -23.72f, 1000.92f},
    {"Four Dragons", 10, 2016.11f, 1017.15f, 996.87f},
    {"Casino Floor (Redsands West)", 12, 1133.34f, -7.84f, 1000.67f},
    {"Caligula's Casino", 1, 2233.93f, 1711.80f, 1011.63f},
    {"Caligula's Roof", 1, 2268.51f, 1647.76f, 1084.23f},
    {"Victim", 5, 225.03f, -9.18f, 1002.21f},
    {"Sub Urban", 1, 204.11f, -46.80f, 1001.80f},
    {"Zip", 18, 161.40f, -94.24f, 1001.80f},
    {"Didier Sachs", 14, 204.16f, -165.76f, 1000.52f},
    {"Binco", 15, 207.52f, -109.74f, 1005.13f},
    {"Pro-Laps", 3, 206.46f, -137.70f, 1003.09f},
    {"Pizza Stack", 5, 372.55f, -131.36f, 1001.49f},
    {"Rusty Brown's Donuts", 17, 378.02f, -190.51f, 1000.63f},
    {"Burger Shot", 10, 366.02f, -73.34f, 1001.50f},
    {"Cluckin' Bell", 9, 366.00f, -9.43f, 1001.85f},
    {"Bar", 11, 501.95f, -70.56f, 998.75f},
    {"Lil' Probe Inn", 18, -227.57f, 1401.55f, 27.76f},
    {"Barber shop 1", 12, 411.97f, -51.92f, 1001.89f},
    {"Barber shop 2", 2, 414.29f, -18.80f, 1001.80f},
    {"Barber shop 3", 3, 418.46f, -80.45f, 1001.80f},
    {"Tattoo parlor", 3, -201.22f, -43.24f, 1002.27f},
    {"Sex shop", 3, -100.26f, -22.93f, 1000.71f},
    {"Burglary house 1", 3, 234.60f, 1187.81f, 1080.25f},
    {"Burglary house 2", 2, 225.57f, 1240.06f, 1082.14f},
    {"Burglary house 3", 1, 224.28f, 1289.19f, 1082.14f},
    {"Burglary house 4", 5, 239.28f, 1114.19f, 1080.99f},
    {"Burglary house 5", 15, 295.13f, 1473.37f, 1080.25f},
    {"Burglary house 6", 2, 446.62f, 1397.73f, 1084.30f},
    {"Burglary house 7", 5, 227.75f, 1114.38f, 1080.99f},
    {"Burglary house 8", 4, 261.11f, 1287.21f, 1080.25f},
    {"Burglary house 9", 10, 24.37f, 1341.18f, 1084.37f},
    {"Burglary house 10", 4, 221.67f, 1142.49f, 1082.60f},
    {"Burglary house 11", 4, -262.17f, 1456.61f, 1084.36f},
    {"Burglary house 12", 5, 22.86f, 1404.91f, 1084.42f},
    {"Burglary house 13", 5, 140.36f, 1367.88f, 1083.86f},
    {"Burglary house 14", 6, 234.28f, 1065.22f, 1084.21f},
    {"Burglary house 15", 6, -68.51f, 1353.84f, 1080.21f},
    {"Burglary house 16", 15, -285.25f, 1471.19f, 1084.37f},
    {"Burglary house 17", 8, -42.52f, 1408.22f, 1084.42f},
    {"Burglary house 18", 9, 84.92f, 1324.29f, 1083.85f},
    {"Burglary house 19", 9, 260.74f, 1238.22f, 1084.25f},
    {"Burglary house 20", 15, 327.80f, 1479.74f, 1084.43f},
    {"Burglary house 21", 15, 295.46f, 1474.69f, 1080.26f},
    {"Burglary house 22", 8, -42.49f, 1407.644f, 1084.43f},
    {"Burglary house 23", 15, 375.57f, 1417.44f, 1081.33f},
    {"Ammu-nation 1", 7, 315.24f, -140.88f, 999.60f},
    {"Ammu-nation 2", 1, 285.83f, -39.01f, 1001.51f},
    {"Ammu-nation 3", 4, 291.76f, -80.13f, 1001.51f},
    {"Ammu-nation 4", 6, 297.14f, -109.87f, 1001.51f},
    {"Ammu-nation 5", 6, 316.50f, -167.62f, 999.59f},
    {"The Johnson house", 3, 2496.05f, -1695.17f, 1014.74f},
    {"Angel Pine trailer", 2, 1.18f, -3.23f, 999.42f},
    {"Abandoned AC tower", 10, 419.89f, 2537.11f, 10.00f},
    {"Wardrobe/Changing room", 14, 256.90f, -41.65f, 1002.02f},
    {"The Camel's Toe safehouse", 1, 2216.12f, -1076.30f, 1050.48f},
    {"Verdant Bluffs safehouse", 8, 2365.10f, -1133.07f, 1050.87f},
    {"Willowfield safehouse", 11, 2282.97f, -1140.28f, 1050.89f},
    {"Vank Hoff Hotel", 5, 2233.69f, -1112.81f, 1050.88f},
    {"Unknown safe house", 9, 2319.12f, -1023.95f, 1050.21f},
    {"Safe House 1", 10, 2262.83f, -1137.71f, 1050.63f},
    {"Safe House 2", 8, 2365.24f, -1134.297f, 1050.88f},
    {"Safe House 3", 6, 2333.033f, -1073.96f, 1049.10f},
    {"Safe House 4", 1, 2216.54f, -1076.29f, 1050.50f},
    {"Safe House 5", 6, 2194.29f, -1204.015f, 1049.10f},
    {"Safe House 6", 6, 2308.87f, -1210.78f, 1049.10f},
    {"Safe House 7", 12, 2324.38f, -1148.48f, 1050.71f},
    {"Denise's house", 1, 245.23f, 304.76f, 999.14f},
    {"Helena's barn", 3, 290.62f, 309.06f, 999.14f},
    {"Barbara's house", 5, 322.50f, 303.69f, 999.14f},
    {"Katie's house", 2, 269.64f, 305.95f, 999.14f},
    {"Michelle's house", 4, 306.19f, 307.81f, 1003.30f},
    {"Planning Department", 3, 386.52f, 173.63f, 1008.38f},
    {"Los Santos Police Department", 6, 246.66f, 65.80f, 1003.64f},
    {"Las Venturas Police Department", 3, 288.47f, 170.06f, 1007.17f},
    {"San Fierro Police Department", 10, 246.06f, 108.97f, 1003.21f},
    {"Oval Stadium", 1, -1402.66f, 106.38f, 1032.27f},
    {"Vice Stadium", 16, -1401.06f, 1265.37f, 1039.86f},
    {"Blood Bowl Stadium", 15, -1417.89f, 932.44f, 1041.53f},
    {"Bike school", 3, 1494.85f, 1306.47f, 1093.29f},
    {"Driving school", 3, -2031.11f, -115.82f, 1035.17f},
    {"Ganton Gym", 5, 770.80f, -0.70f, 1000.72f},
    {"Cobra Gym", 6, 773.88f, -47.76f, 1000.58f},
    {"Below The Belt Gym", 7, 773.73f, -74.69f, 1000.65f},
    {"Brothel 1", 3, 974.01f, -9.59f, 1001.14f},
    {"Brothel 2", 3, 961.93f, -51.90f, 1001.11f},
    {"The Big Spread Ranch", 3, 1212.14f, -28.53f, 1000.95f},
    {"The Pig Pen", 2, 1204.66f, -13.54f, 1000.92f},
    {"Club", 17, 493.14f, -24.26f, 1000.67f},
    {"Fanny Batter's Whore House", 6, 748.46f, 1438.23f, 1102.95f},
    {"Warehouse 1", 18, 1290.41f, 1.95f, 1001.02f},
    {"Warehouse 2", 1, 1412.14f, -2.28f, 1000.92f},
    {"Inside Track Betting", 3, 830.60f, 5.94f, 1004.17f},
    {"Blastin' Fools Records", 3, 1037.82f, 0.39f, 1001.28f},
    {"B Dup's Apartment", 3, 1527.04f, -12.02f, 1002.09f},
    {"B Dup's Crack Palace", 2, 1523.50f, -47.82f, 1002.26f},
    {"OG Loc's House", 3, 512.92f, -11.69f, 1001.56f},
    {"Ryder's house", 2, 2447.87f, -1704.45f, 1013.50f},
    {"Sweet's House", 1, 2527.01f, -1679.20f, 1015.49f},
    {"Wu-Zi Mu's", 1, -2158.67f, 642.09f, 1052.37f},
    {"Los Santos Airport", 14, -1864.94f, 55.73f, 1055.52f},
    {"Four Dragons' Janitor's Office", 10, 1893.07f, 1017.89f, 31.88f},
    {"Jefferson Motel", 15, 2217.28f, -1150.53f, 1025.79f},
    {"Kickstart Stadium", 14, -1420.42f, 1616.92f, 1052.53f},
    {"Liberty City", 1, -741.84f, 493.00f, 1371.97f},
    {"Francis International Airport", 14, -1813.21f, -58.01f, 1058.96f},
    {"The Pleasure Domes", 3, -2638.82f, 1407.33f, 906.46f},
    {"RC Battlefield", 10, -1129.89f, 1057.54f, 1346.41f},
    {"San Fierro Garage", 1, -2041.23f, 178.39f, 28.84f},
    {"The Welcome Pump", 1, 681.62f, -451.89f, -25.61f},
    {"8-Track Stadium", 7, -1403.01f, -250.45f, 1043.53f},
    {"Dirtbike Stadium", 4, -1421.56f, -663.82f, 1059.55f},
    {"Crack Den", 5, 322.11f, 1119.32f, 1083.88f},
    {"Big Smoke's Crack Palace", 2, 2536.53f, -1294.84f, 1044.12f},
    {"Zero's RC Shop", 6, -2240.10f, 136.97f, 1035.41f},
    {"Sherman Dam", 17, -944.24f, 1886.15f, 5.00f},
    {"Rosenberg's Office", 2, 2182.20f, 1628.58f, 1043.87f},
    {"Secret Valley Diner", 6, 442.12f, -52.47f, 999.71f},
    {"World of Coq", 1, 445.60f, -6.98f, 1000.73f},
    {"Jays Diner", 5, 454.98f, -107.25f, 999.43f},
    {"Madd Dogg's Mansion", 5, 1267.84f, -776.95f, 1091.90f},
    {"Colonel Furhberger's", 8, 2807.36f, -1171.70f, 1025.57f},
    {"Burning Desire Building", 5, 2350.15f, -1181.06f, 1027.97f},
    {"Atrium", 18, 1727.28f, -1642.94f, 20.22f},
    {"Sindacco Abatoir", 1, 963.05f, 2159.75f, 1011.03f},
    {"Jet Interior", 1, 1.54f, 23.31f, 1199.59f},
    {"Andromada", 9, 315.45f, 976.59f, 1960.85f},
    {"Palomino Bank", 0, 2306.38f, -15.23f, 26.74f},
    {"Dillimore Gas Station", 0, 663.05f, -573.62f, 16.33f},
    {"Random House", 2, 2236.69f, -1078.94f, 1049.02f},
    {"Budget Inn Motel Room", 12, 446.32f, 509.96f, 1001.41f},
};
constexpr int INTERIOR_SPOT_COUNT = static_cast<int>(sizeof(INTERIOR_SPOTS) / sizeof(INTERIOR_SPOTS[0]));

// Страница списка интерьеров: в тело диалога (~4 КБ) весь список не влезает.
constexpr int INTERIOR_PAGE_SIZE = 40;

// Готовые позы NPC: проверенные зацикленные анимации для типовых сценок.
struct AnimPreset
{
    const char *lib;
    const char *name;
    const char *label; // utf-8, конвертируется при сборке диалога
};
constexpr AnimPreset ANIM_PRESETS[] = {
    {"DEALER", "DEALER_IDLE", "Стоит, торгует"},
    {"COP_AMBIENT", "Coplook_loop", "Стоит, осматривается"},
    {"GANGS", "leanIDLE", "Прислонился к стене"},
    {"SMOKING", "M_smklean_loop", "Курит, прислонившись"},
    {"PED", "phone_talk", "Говорит по телефону"},
    {"BEACH", "ParkSit_M_loop", "Сидит на земле (м)"},
    {"BEACH", "ParkSit_W_loop", "Сидит на земле (ж)"},
    {"SUNBATHE", "Lay_Bac_Loop", "Лежит на спине"},
    {"DANCING", "dnce_M_a", "Танцует"},
    {"DANCING", "DAN_Down_A", "Танцует (низко)"},
    {"BAR", "Barserve_loop", "Бармен за стойкой"},
    {"CASINO", "cards_loop", "Играет в карты (сидя)"},
    {"CRIB", "PED_Console_Loop", "Играет в приставку (сидя)"},
    {"MISC", "Plyrlean_loop", "Облокотился"},
    {"PED", "WOMAN_idlestance", "Стоит (женская поза)"},
    {"SWEET", "Sweet_injuredloop", "Лежит раненый"},
};
constexpr int ANIM_PRESET_COUNT = static_cast<int>(sizeof(ANIM_PRESETS) / sizeof(ANIM_PRESETS[0]));

bool parseFloat(const std::string &text, float &out)
{
    try
    {
        size_t pos = 0;
        out = std::stof(text, &pos);
        return pos > 0;
    }
    catch (...)
    {
        return false;
    }
}

bool parseVec3(const std::string &text, Vector3 &out)
{
    std::istringstream ss(text);
    return static_cast<bool>(ss >> out.x >> out.y >> out.z);
}

float wrapDegrees(float angle)
{
    while (angle < 0.0f)
    {
        angle += 360.0f;
    }
    while (angle >= 360.0f)
    {
        angle -= 360.0f;
    }
    return angle;
}

// Множитель шага по зажатым модификаторам: Sprint — крупно, Alt (walk) — точно.
float stepMultiplier(const PlayerKeyData &keys)
{
    if (keys.keys & KEY_SPRINT)
    {
        return SPRINT_MULTIPLIER;
    }
    if (keys.keys & KEY_WALK)
    {
        return WALK_MULTIPLIER;
    }
    return 1.0f;
}
} // namespace

EditorSystem::EditorSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_commandService(serviceRegister.getService<PlayerCommandService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_checkpointService(serviceRegister.getService<CheckpointService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_objectEditService(serviceRegister.getService<ObjectEditService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
    listen(core.getPlayers().getPlayerUpdateDispatcher(), this);

    m_commandService.add("editor", {},
                         [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                         {
                             EditorState &state = stateOf(player);

                             if (!state.enabled)
                             {
                                 enableEditor(player);
                                 if (stateOf(player).enabled)
                                 {
                                     showMain(player);
                                 }
                                 return;
                             }

                             // Из «ручных» режимов /editor фиксирует сущность и возвращает в её меню.
                             if (state.keyMode != KeyMode::Camera)
                             {
                                 state.keyMode = KeyMode::Camera;
                                 showEntityEdit(player);
                                 return;
                             }
                             if (state.followCamera && selectedValid(state))
                             {
                                 state.followCamera = false;
                                 showEntityEdit(player);
                                 return;
                             }

                             showMain(player);
                         },
                         PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "редактор объектов (меню)",
                         PlayerCommandService::HelpCategory::Hidden);
}

void EditorSystem::initialize(IComponentList *components)
{
    m_objects = components->queryComponent<IObjectsComponent>();
    m_actors = components->queryComponent<IActorsComponent>();
    m_pickups = components->queryComponent<IPickupsComponent>();
}

EditorSystem::EditorState &EditorSystem::stateOf(const IPlayer &player)
{
    return m_state[player.getID()];
}

bool EditorSystem::asyncOpBusy(IPlayer &player)
{
    if (!stateOf(player).asyncOpPending)
    {
        return false;
    }
    player.sendClientMessage(Colour::White(), u("Идёт операция с файлом, подождите"));
    return true;
}

IPlayer *EditorSystem::editorPlayer(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player || !m_state[playerId].enabled)
    {
        return nullptr;
    }
    return player;
}

bool EditorSystem::selectedValid(const EditorState &state) const
{
    return state.selectedIndex >= 0 && state.selectedIndex < (int)state.entities.size();
}

void EditorSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    EditorState &state = stateOf(player);

    // Сущности остаются в мире — но зондируемая запаркована под картой, вернём её.
    cancelGroundProbe(state);
    destroyMouseProxy(state); // прокси-объект мыши — мусор без хозяина
    // Машины сцены остаются — снять с них редакторский байпас античита.
    setVehicleEditBypasses(state, false);

    if (state.cameraObjectId >= 0 && m_objects)
    {
        m_objects->release(state.cameraObjectId);
    }
    state = EditorState{};
}

// ------------------------------------------------------------------ режим

void EditorSystem::enableEditor(IPlayer &player)
{
    if (!m_objects)
    {
        player.sendClientMessage(Colour::White(), u("Компонент объектов недоступен"));
        return;
    }

    EditorState &state = stateOf(player);
    state.returnPosition = m_locationService.getPosition(player.getID());
    state.cameraPosition = state.returnPosition + Vector3(0.0f, 0.0f, 5.0f);

    IObject *camObject = m_objects->create(0, state.cameraPosition, Vector3(0.0f, 0.0f, 0.0f));
    if (!camObject)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось создать объект камеры (лимит пула)"));
        return;
    }

    state.enabled = true;
    state.cameraObjectId = camObject->getID();
    player.attachCameraToObject(*camObject);

    // Замораживать тело нельзя: у замороженного клиента блокируется вращение
    // камеры и синк падает до ~1 Гц (ступенчатое движение). Тело свободно бегает
    // вслепую — валидацию позиции отключаем (FindZ-зонды тоже таскают его по
    // карте), а на выходе вернём его в точку входа.
    m_locationService.setBypass(player.getID(), true);

    player.sendClientMessage(Colour::White(),
                             u("Редактор включён. WASD — полёт, Jump/C — вверх/вниз, Sprint — быстрее. /editor — меню."));
}

void EditorSystem::disableEditor(IPlayer &player)
{
    EditorState &state = stateOf(player);

    cancelGroundProbe(state); // вернуть запаркованную зондом сущность
    destroyMouseProxy(state); // незакрытая сессия прокси-редактирования

    if (state.cameraObjectId >= 0 && m_objects)
    {
        m_objects->release(state.cameraObjectId);
    }
    state.cameraObjectId = -1;
    state.enabled = false;
    state.followCamera = false;
    state.keyMode = KeyMode::Camera;
    state.selectedIndex = -1;
    refreshCheckpointPreview(player); // снять превью чекпоинта

    // Машины сцены становятся обычными мировыми — античит снова их охраняет.
    setVehicleEditBypasses(state, false);
    // Активная сессия нативного редактирования/выбора мышью завершается.
    m_objectEditService.end(player);

    // Возвращаем тело в точку входа: зонды и слепой бег растаскали его по карте.
    m_locationService.teleport(player, state.returnPosition);
    m_locationService.setBypass(player.getID(), false);
    player.setCameraBehind();
    player.sendClientMessage(Colour::White(), u("Редактор выключен. Расставленные сущности остались на сцене."));
}

void EditorSystem::teleportBodyToCamera(IPlayer &player)
{
    EditorState &state = stateOf(player);
    m_locationService.teleport(player, state.cameraPosition);
    // Точка выхода из редактора переезжает сюда — телепорт к камере и задуман
    // как способ переместиться по миру.
    state.returnPosition = state.cameraPosition;
    player.sendClientMessage(Colour::White(),
                             u("Тело телепортировано к камере (упадёт на землю, если камера в воздухе)."));
}

// ------------------------------------------------------------------ per-tick

bool EditorSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    EditorState &state = stateOf(player);
    if (!state.enabled)
    {
        return true;
    }

    processGroundProbe(player);

    // В «ручных» режимах стрелки отданы сущности — камера стоит на месте.
    if (state.keyMode != KeyMode::Camera)
    {
        processKeyMode(player);
        return true;
    }

    processCameraFlight(player);

    // Привязанная к взгляду сущность следует за камерой.
    if (state.followCamera && selectedValid(state))
    {
        EditorEntity &entity = state.entities[state.selectedIndex];
        entity.position = placementPoint(player);
        applyEntityTransform(entity);
        if (entity.type == EntityType::Checkpoint)
        {
            refreshCheckpointPreview(player);
        }
    }

    return true;
}

void EditorSystem::processCameraFlight(IPlayer &player)
{
    EditorState &state = stateOf(player);

    const PlayerAimData &aim = player.getAimData();
    Vector3 forward = aim.camFrontVector;
    const float len = glm::length(forward);
    if (len < 0.0001f)
    {
        return;
    }
    forward /= len;

    const PlayerKeyData &keyData = player.getKeyData();
    const float speed = state.cameraSpeed * ((keyData.keys & KEY_SPRINT) ? SPRINT_MULTIPLIER : 1.0f);
    bool moved = false;

    if (keyData.upDown)
    {
        state.cameraPosition += forward * (keyData.upDown > 0 ? -speed : speed);
        moved = true;
    }

    if (keyData.leftRight)
    {
        Vector3 right = glm::vec3(forward.y, -forward.x, 0.0f);
        const float rlen = glm::length(right);
        if (rlen > 0.0001f)
        {
            right /= rlen;
            state.cameraPosition += right * (keyData.leftRight > 0 ? speed : -speed);
            moved = true;
        }
    }

    if (keyData.keys & KEY_JUMP)
    {
        state.cameraPosition.z += speed;
        moved = true;
    }
    if (keyData.keys & KEY_CROUCH)
    {
        state.cameraPosition.z -= speed;
        moved = true;
    }

    if (moved && state.cameraObjectId >= 0 && m_objects)
    {
        if (IObject *camObject = m_objects->get(state.cameraObjectId))
        {
            camObject->setPosition(state.cameraPosition);
        }
    }
}

void EditorSystem::processKeyMode(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (!selectedValid(state))
    {
        state.keyMode = KeyMode::Camera;
        return;
    }

    const PlayerKeyData &keyData = player.getKeyData();
    const float step = state.keyStep * stepMultiplier(keyData);
    EditorEntity &entity = state.entities[state.selectedIndex];
    bool changed = false;

    switch (state.keyMode)
    {
    case KeyMode::MoveXY:
    {
        // Движение относительно взгляда: «вперёд» — куда смотрит камера (по горизонтали).
        Vector3 forward = player.getAimData().camFrontVector;
        forward.z = 0.0f;
        const float len = glm::length(forward);
        if (len < 0.0001f)
        {
            break;
        }
        forward /= len;
        const Vector3 right{forward.y, -forward.x, 0.0f};

        if (keyData.upDown < 0)
        {
            entity.position += forward * step;
            changed = true;
        }
        else if (keyData.upDown > 0)
        {
            entity.position -= forward * step;
            changed = true;
        }
        if (keyData.leftRight > 0)
        {
            entity.position += right * step;
            changed = true;
        }
        else if (keyData.leftRight < 0)
        {
            entity.position -= right * step;
            changed = true;
        }
        break;
    }
    case KeyMode::MoveZ:
        if (keyData.upDown < 0)
        {
            entity.position.z += step;
            changed = true;
        }
        else if (keyData.upDown > 0)
        {
            entity.position.z -= step;
            changed = true;
        }
        break;
    case KeyMode::Rotate:
    {
        // Для чекпоинта вращения нет — влево/вправо меняют радиус.
        if (entity.type == EntityType::Checkpoint)
        {
            if (keyData.leftRight > 0)
            {
                entity.radius += step;
                changed = true;
            }
            else if (keyData.leftRight < 0)
            {
                entity.radius -= step;
                changed = true;
            }
            entity.radius = std::clamp(entity.radius, CheckpointService::MIN_RADIUS, CheckpointService::MAX_RADIUS);
            break;
        }

        const float rotStep = step * ROTATE_DEG_PER_STEP;
        if (keyData.leftRight > 0)
        {
            entity.rotation.z = wrapDegrees(entity.rotation.z - rotStep);
            changed = true;
        }
        else if (keyData.leftRight < 0)
        {
            entity.rotation.z = wrapDegrees(entity.rotation.z + rotStep);
            changed = true;
        }
        break;
    }
    default:
        break;
    }

    if (changed)
    {
        applyEntityTransform(entity);
        if (entity.type == EntityType::Checkpoint)
        {
            if (--state.checkpointRefreshCooldown <= 0)
            {
                state.checkpointRefreshCooldown = CHECKPOINT_REFRESH_TICKS;
                refreshCheckpointPreview(player);
            }
        }
    }
}

Vector3 EditorSystem::placementPoint(IPlayer &player) const
{
    const PlayerAimData &aim = player.getAimData();
    Vector3 forward = aim.camFrontVector;
    const float len = glm::length(forward);
    if (len < 0.0001f)
    {
        return m_locationService.getPosition(player.getID());
    }
    forward /= len;

    const EditorState &state = m_state[player.getID()];
    return aim.camPos + forward * state.placeDistance;
}

// ------------------------------------------------------------------ сущности

void EditorSystem::createObjectEntity(IPlayer &player, int model)
{
    if (asyncOpBusy(player))
    {
        return;
    }
    EditorState &state = stateOf(player);
    const Vector3 pos = placementPoint(player);

    IObject *object = m_objects ? m_objects->create(model, pos, Vector3(0.0f, 0.0f, 0.0f)) : nullptr;
    if (!object)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось создать объект"));
        return;
    }

    EditorEntity entity;
    entity.type = EntityType::Object;
    entity.entityId = object->getID();
    entity.model = model;
    entity.position = pos;

    state.entities.push_back(entity);
    state.selectedIndex = static_cast<int>(state.entities.size()) - 1;
    state.followCamera = false;

    if (state.autoGround)
    {
        requestGroundSnap(player, state.selectedIndex);
    }
}

void EditorSystem::destroyMouseProxy(EditorState &state)
{
    if (state.mouseProxyObjectId >= 0 && m_objects)
    {
        m_objects->release(state.mouseProxyObjectId);
    }
    state.mouseProxyObjectId = -1;
    state.mouseProxyIndex = -1;
    state.mouseProxyEntityId = -1;
}

void EditorSystem::beginMouseEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (!selectedValid(state))
    {
        showMain(player);
        return;
    }

    state.followCamera = false;
    state.keyMode = KeyMode::Camera;

    if (state.entities[state.selectedIndex].type != EntityType::Object)
    {
        beginProxyMouseEdit(player);
        return;
    }

    IObject *object = m_objects ? m_objects->get(state.entities[state.selectedIndex].entityId) : nullptr;
    if (!object)
    {
        showEntityEdit(player);
        return;
    }

    m_objectEditService.beginEdit(
        player, *object,
        [this, playerId = player.getID()](IPlayer &p, IObject &object, ObjectEditResponse response, Vector3 position,
                                          Vector3 rotation)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            // Сущность ищем по pool id: индексы могли сдвинуться за время сессии.
            EditorState &state = m_state[playerId];
            for (EditorEntity &entity : state.entities)
            {
                if (entity.type != EntityType::Object || entity.entityId != object.getID())
                {
                    continue;
                }

                if (response == ObjectEditResponse_Cancel)
                {
                    applyEntityTransform(entity); // откат на сохранённый трансформ
                }
                else
                {
                    entity.position = position;
                    entity.rotation = rotation;
                    applyEntityTransform(entity); // Update — двигается у всех вживую
                }
                break;
            }

            if (response == ObjectEditResponse_Final || response == ObjectEditResponse_Cancel)
            {
                showEntityEdit(*player);
            }
        });

    player.sendClientMessage(Colour::White(),
                             u("Тащите стрелки/кольца мышью. Клик вне объекта — сохранить, ESC — отмена."));
}

void EditorSystem::beginProxyMouseEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (!m_objects || !selectedValid(state))
    {
        showMain(player);
        return;
    }
    destroyMouseProxy(state); // прошлая сессия могла не закрыться

    EditorEntity &selected = state.entities[state.selectedIndex];
    // Пикапу прокси с его же моделью — выглядит как сам пикап; актору/машине/
    // чекпоинту — маркер-стрелка.
    const int proxyModel = selected.type == EntityType::Pickup ? selected.model : MOUSE_PROXY_MARKER_MODEL;
    IObject *proxy = m_objects->create(proxyModel, selected.position, Vector3(0.0f, 0.0f, selected.rotation.z));
    if (!proxy)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось создать прокси-объект (лимит пула)"));
        showEntityEdit(player);
        return;
    }

    state.mouseProxyObjectId = proxy->getID();
    state.mouseProxyIndex = state.selectedIndex;
    state.mouseProxyEntityId = selected.entityId;
    state.mouseProxyType = selected.type;
    state.mouseProxyOrigPos = selected.position;
    state.mouseProxyOrigAngle = selected.rotation.z;

    m_objectEditService.beginEdit(
        player, *proxy,
        [this, playerId = player.getID()](IPlayer &, IObject &object, ObjectEditResponse response, Vector3 position,
                                          Vector3 rotation)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }
            EditorState &state = m_state[playerId];
            if (object.getID() != state.mouseProxyObjectId)
            {
                return; // сессия другого прокси
            }

            // Сущность по снапшоту: индексы могли сдвинуться (удаление и т.п.).
            EditorEntity *entity = nullptr;
            if (state.mouseProxyIndex >= 0 && state.mouseProxyIndex < (int)state.entities.size())
            {
                EditorEntity &candidate = state.entities[state.mouseProxyIndex];
                if (candidate.type == state.mouseProxyType && candidate.entityId == state.mouseProxyEntityId)
                {
                    entity = &candidate;
                }
            }
            if (!entity)
            {
                destroyMouseProxy(state);
                m_objectEditService.end(*player);
                showMain(*player);
                return;
            }

            if (response == ObjectEditResponse_Update)
            {
                // Живое применение только дешёвым типам (setPosition без
                // рестрима); пикап и чекпоинт переезжают на сохранении —
                // их перенос рестримит сущность всем вокруг.
                if (entity->type == EntityType::Actor || entity->type == EntityType::Vehicle)
                {
                    entity->position = position;
                    entity->rotation.z = rotation.z;
                    applyEntityTransform(*entity);
                }
                return;
            }

            if (response == ObjectEditResponse_Final)
            {
                entity->position = position;
                if (entity->type == EntityType::Actor || entity->type == EntityType::Vehicle)
                {
                    entity->rotation.z = rotation.z;
                }
                applyEntityTransform(*entity);
            }
            else // Cancel — откат (живые типы успели подвигаться)
            {
                entity->position = state.mouseProxyOrigPos;
                entity->rotation.z = state.mouseProxyOrigAngle;
                applyEntityTransform(*entity);
            }
            refreshCheckpointPreview(*player);
            destroyMouseProxy(state);
            showEntityEdit(*player);
        });

    player.sendClientMessage(
        Colour::White(), u("Тащите прокси-маркер мышью. Клик вне объекта — сохранить (сущность переедет), ESC — отмена."));
}

void EditorSystem::beginMouseSelect(IPlayer &player)
{
    m_objectEditService.beginSelect(
        player,
        [this, playerId = player.getID()](IPlayer &, IObject &object, int, Vector3)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            for (std::size_t i = 0; i < state.entities.size(); ++i)
            {
                if (state.entities[i].type == EntityType::Object && state.entities[i].entityId == object.getID())
                {
                    state.selectedIndex = static_cast<int>(i);
                    showEntityEdit(*player);
                    return;
                }
            }
            player->sendClientMessage(Colour::White(), u("Этот объект не из сцены редактора"));
        });

    player.sendClientMessage(Colour::White(), u("Кликните по объекту сцены. ESC — отмена."));
}

void EditorSystem::createActorEntity(IPlayer &player, int skin)
{
    if (asyncOpBusy(player))
    {
        return;
    }
    EditorState &state = stateOf(player);
    const Vector3 pos = placementPoint(player);

    IActor *actor = m_actors ? m_actors->create(skin, pos, 0.0f) : nullptr;
    if (!actor)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось создать актора (нет компонента или лимит)"));
        return;
    }

    EditorEntity entity;
    entity.type = EntityType::Actor;
    entity.entityId = actor->getID();
    entity.model = skin;
    entity.position = pos;

    state.entities.push_back(entity);
    state.selectedIndex = static_cast<int>(state.entities.size()) - 1;
    state.followCamera = false;

    if (state.autoGround)
    {
        requestGroundSnap(player, state.selectedIndex);
    }
}

void EditorSystem::setVehicleEditBypasses(EditorState &state, bool enable)
{
    for (const EditorEntity &entity : state.entities)
    {
        if (entity.type == EntityType::Vehicle)
        {
            m_vehicleService.setEditBypass(entity.entityId, enable);
        }
    }
}

IVehicle *EditorSystem::spawnVehicle(const EditorEntity &entity)
{
    // Создание — через единый API VehicleService (источник правды о машинах):
    // расставленная редактором машина без владельца и без авто-респауна (как было).
    return m_vehicleService.create(entity.model, entity.position, entity.rotation.z, entity.colour1, entity.colour2,
                                   VehicleService::Owner::None, -1);
}

void EditorSystem::createVehicleEntity(IPlayer &player, int model)
{
    if (asyncOpBusy(player))
    {
        return;
    }
    EditorState &state = stateOf(player);

    EditorEntity entity;
    entity.type = EntityType::Vehicle;
    entity.model = model;
    entity.position = placementPoint(player);

    IVehicle *vehicle = spawnVehicle(entity);
    if (!vehicle)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось создать машину (нет компонента или лимит)"));
        return;
    }
    entity.entityId = vehicle->getID();
    // Редактор двигает машину серверно, а тело редактора далеко от камеры —
    // её unoccupied-синк не валидируем, иначе античит кикнет самого маппера.
    m_vehicleService.setEditBypass(entity.entityId, true);

    state.entities.push_back(entity);
    state.selectedIndex = static_cast<int>(state.entities.size()) - 1;
    state.followCamera = false;

    if (state.autoGround)
    {
        requestGroundSnap(player, state.selectedIndex);
    }
}

void EditorSystem::createPickupEntity(IPlayer &player, int model)
{
    if (asyncOpBusy(player))
    {
        return;
    }
    EditorState &state = stateOf(player);
    const Vector3 pos = placementPoint(player);

    // isStatic=false: позицию можно менять на месте (setPosition с рестримом).
    IPickup *pickup = m_pickups ? m_pickups->create(model, DEFAULT_PICKUP_TYPE, pos, 0, false) : nullptr;
    if (!pickup)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось создать пикап (нет компонента или лимит)"));
        return;
    }

    EditorEntity entity;
    entity.type = EntityType::Pickup;
    entity.entityId = pickup->getID();
    entity.model = model;
    entity.position = pos;
    entity.pickupType = DEFAULT_PICKUP_TYPE;

    state.entities.push_back(entity);
    state.selectedIndex = static_cast<int>(state.entities.size()) - 1;
    state.followCamera = false;

    if (state.autoGround)
    {
        requestGroundSnap(player, state.selectedIndex);
    }
}

void EditorSystem::createCheckpointEntity(IPlayer &player)
{
    if (asyncOpBusy(player))
    {
        return;
    }
    EditorState &state = stateOf(player);

    EditorEntity entity;
    entity.type = EntityType::Checkpoint;
    entity.position = placementPoint(player);
    entity.radius = DEFAULT_CHECKPOINT_RADIUS;

    // Реальный глобальный чекпоинт (без обработчиков): остаётся в мире после
    // выхода из редактора и стримится всем игрокам, как и остальные сущности.
    entity.entityId = m_checkpointService.add(entity.position, entity.radius, nullptr);
    if (entity.entityId < 0)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось создать чекпоинт"));
        return;
    }

    state.entities.push_back(entity);
    state.selectedIndex = static_cast<int>(state.entities.size()) - 1;
    state.followCamera = false;

    refreshCheckpointPreview(player);
    if (state.autoGround)
    {
        requestGroundSnap(player, state.selectedIndex);
    }
}

void EditorSystem::refreshCheckpointPreview(IPlayer &player)
{
    EditorState &state = stateOf(player);
    const bool active =
        state.enabled && selectedValid(state) && state.entities[state.selectedIndex].type == EntityType::Checkpoint;

    if (!active)
    {
        if (state.previewIndex != -1)
        {
            state.previewIndex = -1;
            m_checkpointService.clearForPlayer(player); // стрим глобальных вернёт ближайший
        }
        return;
    }

    // Личный чекпоинт поверх стрима: выбранный виден, даже если рядом есть более
    // близкий глобальный. Пересоздание маркера — только при фактических изменениях,
    // иначе он мерцал бы на каждом открытии диалога и тике followCamera.
    const EditorEntity &e = state.entities[state.selectedIndex];
    if (state.previewIndex == state.selectedIndex && state.previewPosition == e.position &&
        state.previewRadius == e.radius)
    {
        return;
    }
    state.previewIndex = state.selectedIndex;
    state.previewPosition = e.position;
    state.previewRadius = e.radius;
    m_checkpointService.setForPlayer(player, e.position, e.radius);
}

void EditorSystem::duplicateEntity(IPlayer &player, int index)
{
    if (asyncOpBusy(player))
    {
        return;
    }
    EditorState &state = stateOf(player);
    if (index < 0 || index >= (int)state.entities.size())
    {
        return;
    }

    EditorEntity copy = state.entities[index];
    copy.position += Vector3(1.0f, 1.0f, 0.0f); // рядом, чтобы копия не слилась с оригиналом

    if (copy.type == EntityType::Object)
    {
        IObject *object = m_objects ? m_objects->create(copy.model, copy.position, copy.rotation) : nullptr;
        if (!object)
        {
            player.sendClientMessage(Colour::White(), u("Не удалось создать копию объекта"));
            return;
        }
        copy.entityId = object->getID();
    }
    else if (copy.type == EntityType::Actor)
    {
        IActor *actor = m_actors ? m_actors->create(copy.model, copy.position, copy.rotation.z) : nullptr;
        if (!actor)
        {
            player.sendClientMessage(Colour::White(), u("Не удалось создать копию актора"));
            return;
        }
        copy.entityId = actor->getID();
    }
    else if (copy.type == EntityType::Vehicle)
    {
        IVehicle *vehicle = spawnVehicle(copy);
        if (!vehicle)
        {
            player.sendClientMessage(Colour::White(), u("Не удалось создать копию машины"));
            return;
        }
        copy.entityId = vehicle->getID();
        m_vehicleService.setEditBypass(copy.entityId, true);
    }
    else if (copy.type == EntityType::Pickup)
    {
        IPickup *pickup =
            m_pickups ? m_pickups->create(copy.model, static_cast<PickupType>(copy.pickupType), copy.position, 0, false)
                      : nullptr;
        if (!pickup)
        {
            player.sendClientMessage(Colour::White(), u("Не удалось создать копию пикапа"));
            return;
        }
        copy.entityId = pickup->getID();
    }
    else
    {
        copy.entityId = m_checkpointService.add(copy.position, copy.radius, nullptr);
        if (copy.entityId < 0)
        {
            player.sendClientMessage(Colour::White(), u("Не удалось создать копию чекпоинта"));
            return;
        }
    }

    state.entities.push_back(copy);
    state.selectedIndex = static_cast<int>(state.entities.size()) - 1;
    state.followCamera = false;
    applyEntityAnimation(state.entities.back());
    refreshCheckpointPreview(player); // выбор сменился — превью следует за ним
}

void EditorSystem::applyEntityTransform(EditorEntity &entity)
{
    if (entity.type == EntityType::Object)
    {
        if (IObject *object = m_objects ? m_objects->get(entity.entityId) : nullptr)
        {
            object->setPosition(entity.position);
            object->setRotation(GTAQuat(entity.rotation));
        }
    }
    else if (entity.type == EntityType::Actor)
    {
        if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
        {
            actor->setPosition(entity.position);
            actor->setRotation(GTAQuat(entity.rotation));
        }
    }
    else if (entity.type == EntityType::Vehicle)
    {
        if (IVehicle *vehicle = m_vehicleService.get(entity.entityId))
        {
            vehicle->setPosition(entity.position);
            vehicle->setZAngle(entity.rotation.z);
        }
    }
    else if (entity.type == EntityType::Pickup)
    {
        if (IPickup *pickup = m_pickups ? m_pickups->get(entity.entityId) : nullptr)
        {
            pickup->setPosition(entity.position); // пикапы не вращаются — только позиция
        }
    }
    else
    {
        // Глобальный чекпоинт; превью редактирующего обновляет refreshCheckpointPreview.
        m_checkpointService.update(entity.entityId, entity.position, entity.radius);
    }
}

void EditorSystem::applyEntityAnimation(EditorEntity &entity)
{
    if (entity.type != EntityType::Actor || entity.animLib.empty() || entity.animName.empty())
    {
        return;
    }
    if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
    {
        // Зацикленная — крутится вечно; стоп-поза — проигрывается один раз и
        // замирает на последнем кадре (сидит, лежит и т.п.).
        const bool loop = entity.animLoop;
        actor->applyAnimation(AnimationData(4.1f, loop, true, true, !loop, 0, entity.animLib, entity.animName));
    }
}

bool EditorSystem::setActorAnimation(IPlayer &player, EditorEntity &entity, std::string lib, std::string name,
                                     bool loop)
{
    if (!animationNameValid(lib, name))
    {
        player.sendClientMessage(Colour::White(), u(fmt::format("Анимации {}:{} не существует", lib, name)));
        return false;
    }

    entity.animLib = std::move(lib);
    entity.animName = std::move(name);
    entity.animLoop = loop;
    applyEntityAnimation(entity);
    return true;
}

void EditorSystem::deleteEntity(IPlayer &player, int index)
{
    EditorState &state = stateOf(player);
    if (index < 0 || index >= (int)state.entities.size())
    {
        return;
    }

    // Индексы сдвинутся — сперва вернуть запаркованную зондом сущность и снять зонд.
    cancelGroundProbe(state);

    EditorEntity &entity = state.entities[index];
    if (entity.type == EntityType::Object)
    {
        if (m_objects)
        {
            m_objects->release(entity.entityId);
        }
    }
    else if (entity.type == EntityType::Actor)
    {
        if (m_actors)
        {
            m_actors->release(entity.entityId);
        }
    }
    else if (entity.type == EntityType::Vehicle)
    {
        m_vehicleService.destroy(entity.entityId);
    }
    else if (entity.type == EntityType::Pickup)
    {
        if (m_pickups)
        {
            m_pickups->release(entity.entityId);
        }
    }
    else
    {
        m_checkpointService.remove(entity.entityId);
    }

    state.entities.erase(state.entities.begin() + index);
    state.selectedIndex = -1;
    state.followCamera = false;
    state.keyMode = KeyMode::Camera;
    refreshCheckpointPreview(player); // выбор сброшен — превью гаснет
}

void EditorSystem::clearScene(IPlayer &player)
{
    EditorState &state = stateOf(player);
    cancelGroundProbe(state);

    for (const EditorEntity &entity : state.entities)
    {
        if (entity.type == EntityType::Object)
        {
            if (m_objects)
            {
                m_objects->release(entity.entityId);
            }
        }
        else if (entity.type == EntityType::Actor)
        {
            if (m_actors)
            {
                m_actors->release(entity.entityId);
            }
        }
        else if (entity.type == EntityType::Vehicle)
        {
            m_vehicleService.destroy(entity.entityId);
        }
        else if (entity.type == EntityType::Pickup)
        {
            if (m_pickups)
            {
                m_pickups->release(entity.entityId);
            }
        }
        else
        {
            m_checkpointService.remove(entity.entityId);
        }
    }
    state.entities.clear();
    state.selectedIndex = -1;
    state.followCamera = false;
    state.keyMode = KeyMode::Camera;
    refreshCheckpointPreview(player);
}

// Запускает асинхронный поиск земли: телепортирует невидимое тело игрока в (x, y, высоко),
// клиент сам опустит его на землю и пришлёт Z обычным синком (читаем в processGroundProbe).
void EditorSystem::requestGroundSnap(IPlayer &player, int index)
{
    EditorState &state = stateOf(player);
    if (index < 0 || index >= (int)state.entities.size())
    {
        return;
    }

    // Если уже идёт зонд по другой сущности — вернём её на место, чтобы не осталась под картой.
    if (state.groundProbe >= 0 && state.groundProbe != index && state.groundProbe < (int)state.entities.size())
    {
        applyEntityTransform(state.entities[state.groundProbe]);
    }

    EditorEntity &entity = state.entities[index];
    const Vector3 target = entity.position;

    // Убираем саму сущность из пути рейкаста FindZ, иначе клиент «приземлит» тело игрока на её
    // собственную коллизию и Z будет расти с каждым снапом (объект карабкается сам по себе).
    const Vector3 parkPosition(target.x, target.y, PROBE_PARK_Z);
    if (entity.type == EntityType::Object)
    {
        if (IObject *object = m_objects ? m_objects->get(entity.entityId) : nullptr)
        {
            object->setPosition(parkPosition);
        }
    }
    else if (entity.type == EntityType::Actor)
    {
        if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
        {
            actor->setPosition(parkPosition);
        }
    }
    else if (entity.type == EntityType::Vehicle)
    {
        if (IVehicle *vehicle = m_vehicleService.get(entity.entityId))
        {
            vehicle->setPosition(parkPosition);
        }
    }
    else if (entity.type == EntityType::Pickup)
    {
        if (IPickup *pickup = m_pickups ? m_pickups->get(entity.entityId) : nullptr)
        {
            pickup->setPosition(parkPosition);
        }
    }
    // Checkpoint: коллизии нет — прятать от рейкаста FindZ нечего.

    state.groundProbe = index;
    state.groundProbeTicks = 0;
    player.setPositionFindZ(Vector3(target.x, target.y, GROUND_PROBE_Z));
}

void EditorSystem::cancelGroundProbe(EditorState &state)
{
    if (state.groundProbe >= 0 && state.groundProbe < (int)state.entities.size())
    {
        // Зондируемая сущность запаркована на PROBE_PARK_Z — вернуть на сохранённую позицию.
        applyEntityTransform(state.entities[state.groundProbe]);
    }
    state.groundProbe = -1;
    state.groundProbeTicks = 0;
}

void EditorSystem::processGroundProbe(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (state.groundProbe < 0)
    {
        return;
    }

    // Ждём, пока клиент обработает телепорт + FindZ и пришлёт скорректированную позицию.
    if (++state.groundProbeTicks < GROUND_PROBE_WAIT)
    {
        return;
    }

    if (state.groundProbe >= (int)state.entities.size())
    {
        state.groundProbe = -1;
        return;
    }

    EditorEntity &entity = state.entities[state.groundProbe];
    // В режиме редактора сервис принимает позицию каждый апдейт (байпас) —
    // здесь это уже приземлённый клиентом Z после FindZ.
    const float z = m_locationService.getPosition(player.getID()).z;

    if (z < GROUND_PROBE_Z - 1.0f)
    {
        // Нашли землю. z — это туловище стоящего игрока, уровень земли ниже на
        // PED_ORIGIN_HEIGHT.
        const float groundZ = z - PED_ORIGIN_HEIGHT;
        switch (entity.type)
        {
        case EntityType::Object:
            entity.position.z = groundZ; // origin большинства объектов — у основания
            break;
        case EntityType::Actor:
            entity.position.z = groundZ + PED_ORIGIN_HEIGHT; // origin актора — такое же туловище
            break;
        case EntityType::Vehicle:
            entity.position.z = groundZ + VEHICLE_GROUND_OFFSET;
            break;
        case EntityType::Pickup:
            entity.position.z = groundZ + PICKUP_GROUND_OFFSET;
            break;
        case EntityType::Checkpoint:
            entity.position.z = groundZ + CHECKPOINT_GROUND_OFFSET;
            break;
        }
    }
    else
    {
        // FindZ не сработал (зона не прогружена на клиенте) — оставляем прежнюю высоту.
        player.sendClientMessage(Colour::White(),
                                 u("Не удалось найти землю: зона не прогружена. Подлетите ближе и повторите."));
    }

    applyEntityTransform(entity); // поднять сущность из укрытия на найденную землю (или вернуть как было)
    if (entity.type == EntityType::Checkpoint)
    {
        refreshCheckpointPreview(player);
    }
    state.groundProbe = -1;
}

// ------------------------------------------------------------------ экраны диалогов

void EditorSystem::showMain(IPlayer &player)
{
    EditorState &state = stateOf(player);

    size_t objectCount = 0;
    size_t actorCount = 0;
    size_t vehicleCount = 0;
    size_t pickupCount = 0;
    size_t checkpointCount = 0;
    for (const EditorEntity &e : state.entities)
    {
        switch (e.type)
        {
        case EntityType::Object:
            ++objectCount;
            break;
        case EntityType::Actor:
            ++actorCount;
            break;
        case EntityType::Vehicle:
            ++vehicleCount;
            break;
        case EntityType::Pickup:
            ++pickupCount;
            break;
        case EntityType::Checkpoint:
            ++checkpointCount;
            break;
        }
    }

    std::string body;
    body += "Создать объект\n";
    body += "Создать актора (NPC)\n";
    body += "Создать машину\n";
    body += "Создать пикап\n";
    body += "Создать чекпоинт\n";
    body += fmt::format("Объекты ({})\n", objectCount);
    body += fmt::format("Акторы ({})\n", actorCount);
    body += fmt::format("Машины ({})\n", vehicleCount);
    body += fmt::format("Пикапы ({})\n", pickupCount);
    body += fmt::format("Чекпоинты ({})\n", checkpointCount);
    body += "Выбрать объект мышью\n";
    body += "Телепорт тела к камере\n";
    body += fmt::format("Интерьер: {}\n", m_locationService.getInterior(player.getID()));
    body += "Телепорт по интерьерам\n";
    body += fmt::format("Автоснап к земле: {}\n", state.autoGround ? "ВКЛ" : "выкл");
    body += fmt::format("Скорость камеры: {:.1f}\n", state.cameraSpeed);
    body += "Сохранить в файл\n";
    body += "Загрузить из файла\n";
    body += "Очистить сцену\n";
    body += "Закрыть меню (летать)\n";
    body += "Выйти из редактора";

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Редактор карты", body, "Выбрать", "Закрыть"),
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 return; // закрыли меню — летаем
                             }

                             switch (listItem)
                             {
                             case 0:
                                 showObjectModelInput(*player);
                                 break;
                             case 1:
                                 showActorSkinInput(*player);
                                 break;
                             case 2:
                                 showVehicleModelInput(*player);
                                 break;
                             case 3:
                                 showPickupModelInput(*player);
                                 break;
                             case 4:
                                 createCheckpointEntity(*player);
                                 showEntityEdit(*player);
                                 break;
                             case 5:
                                 showObjectList(*player);
                                 break;
                             case 6:
                                 showActorList(*player);
                                 break;
                             case 7:
                                 showVehicleList(*player);
                                 break;
                             case 8:
                                 showPickupList(*player);
                                 break;
                             case 9:
                                 showCheckpointList(*player);
                                 break;
                             case 10:
                                 beginMouseSelect(*player); // клик по объекту сцены — в его меню
                                 break;
                             case 11:
                                 teleportBodyToCamera(*player);
                                 showMain(*player);
                                 break;
                             case 12:
                                 showInteriorInput(*player);
                                 break;
                             case 13:
                                 showInteriorList(*player);
                                 break;
                             case 14:
                                 m_state[playerId].autoGround = !m_state[playerId].autoGround;
                                 showMain(*player);
                                 break;
                             case 15:
                                 showCameraSpeedInput(*player);
                                 break;
                             case 16:
                                 showSaveNameInput(*player);
                                 break;
                             case 17:
                                 listMapFilesAsync(*player); // листинг каталога на воркере
                                 break;
                             case 18:
                                 showClearConfirm(*player);
                                 break;
                             case 19:
                                 break; // летать
                             case 20:
                                 disableEditor(*player);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void EditorSystem::showEntityEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (!selectedValid(state))
    {
        showMain(player);
        return;
    }

    switch (state.entities[state.selectedIndex].type)
    {
    case EntityType::Actor:
        showActorEdit(player);
        break;
    case EntityType::Vehicle:
        showVehicleEdit(player);
        break;
    case EntityType::Pickup:
        showPickupEdit(player);
        break;
    case EntityType::Checkpoint:
        showCheckpointEdit(player);
        break;
    default:
        showObjectEdit(player);
        break;
    }
}

void EditorSystem::showObjectEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (!selectedValid(state))
    {
        showMain(player);
        return;
    }
    const EditorEntity &e = state.entities[state.selectedIndex];
    refreshCheckpointPreview(player); // выбор мог смениться с чекпоинта или на него

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
    body += "Редактировать мышью (стрелки-оси)\n";
    body += "Двигать клавишами (XY)\n";
    body += "Двигать клавишами (высота)\n";
    body += "Вращать клавишами (Z)\n";
    body += fmt::format("Шаг клавиш: {:.2f}\n", state.keyStep);
    body += "Снэп к земле (FindZ)\n";
    body += fmt::format("Дистанция установки: {:.1f}\n", state.placeDistance);
    body += fmt::format("Позиция: {:.1f} {:.1f} {:.1f}\n", e.position.x, e.position.y, e.position.z);
    body += fmt::format("Поворот: {:.1f} {:.1f} {:.1f}\n", e.rotation.x, e.rotation.y, e.rotation.z);
    body += fmt::format("Сменить модель ({})\n", e.model);
    body += "Дублировать\n";
    body += "Перелететь к объекту\n";
    body += "Удалить";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Объект (модель {})", e.model), body, "Выбрать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            if (response == DialogResponse_Right || !selectedValid(state))
            {
                showMain(*player);
                return;
            }
            EditorEntity &entity = state.entities[state.selectedIndex];

            switch (listItem)
            {
            case 0: // поставить по взгляду
                entity.position = placementPoint(*player);
                applyEntityTransform(entity);
                if (state.autoGround)
                {
                    requestGroundSnap(*player, state.selectedIndex);
                }
                showObjectEdit(*player);
                break;
            case 1: // следовать за взглядом
                state.followCamera = !state.followCamera;
                if (state.followCamera)
                {
                    player->sendClientMessage(
                        Colour::White(), u("Объект следует за взглядом. Летайте, затем /editor чтобы зафиксировать."));
                }
                else
                {
                    showObjectEdit(*player);
                }
                break;
            case 2: // нативный редактор мышью
                beginMouseEdit(*player);
                break;
            case 3:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveXY;
                player->sendClientMessage(Colour::White(),
                                          u("Стрелки двигают объект (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 4:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveZ;
                player->sendClientMessage(
                    Colour::White(), u("Вверх/вниз меняют высоту (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 5:
                state.followCamera = false;
                state.keyMode = KeyMode::Rotate;
                player->sendClientMessage(
                    Colour::White(), u("Влево/вправо вращают объект (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 6:
                showKeyStepInput(*player);
                break;
            case 7: // снэп к земле
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под объектом..."));
                showObjectEdit(*player);
                break;
            case 8:
                showDistanceInput(*player);
                break;
            case 9:
                showPositionInput(*player);
                break;
            case 10:
                showRotationInput(*player);
                break;
            case 11:
                showChangeModelInput(*player);
                break;
            case 12:
                duplicateEntity(*player, state.selectedIndex);
                showEntityEdit(*player);
                break;
            case 13: // перелететь
                state.cameraPosition = entity.position + Vector3(2.0f, 2.0f, 2.0f);
                if (state.cameraObjectId >= 0 && m_objects)
                {
                    if (IObject *camObject = m_objects->get(state.cameraObjectId))
                    {
                        camObject->setPosition(state.cameraPosition);
                    }
                }
                showObjectEdit(*player);
                break;
            case 14:
                deleteEntity(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Объект удалён"));
                showMain(*player);
                break;
            default:
                break;
            }
        });
}

void EditorSystem::showActorEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (!selectedValid(state))
    {
        showMain(player);
        return;
    }
    const EditorEntity &e = state.entities[state.selectedIndex];
    refreshCheckpointPreview(player); // выбор мог смениться с чекпоинта или на него

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
    body += "Редактировать мышью (прокси-маркер)\n";
    body += "Двигать клавишами (XY)\n";
    body += "Двигать клавишами (высота)\n";
    body += "Вращать клавишами (угол)\n";
    body += fmt::format("Шаг клавиш: {:.2f}\n", state.keyStep);
    body += "Снэп к земле (FindZ)\n";
    body += fmt::format("Дистанция установки: {:.1f}\n", state.placeDistance);
    body += fmt::format("Позиция: {:.1f} {:.1f} {:.1f}\n", e.position.x, e.position.y, e.position.z);
    body += fmt::format("Угол: {:.1f}\n", e.rotation.z);
    body += fmt::format("Сменить скин ({})\n", e.model);
    body += fmt::format("Анимация: {}\n", e.animLib.empty() ? "нет" : e.animLib + ":" + e.animName);
    body += fmt::format("Режим анимации: {}\n", e.animLoop ? "зациклена" : "стоп-поза");
    body += "Очистить анимацию\n";
    body += "Дублировать\n";
    body += "Перелететь к актору\n";
    body += "Удалить";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Актор (скин {})", e.model), body, "Выбрать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            if (response == DialogResponse_Right || !selectedValid(state))
            {
                showMain(*player);
                return;
            }
            EditorEntity &entity = state.entities[state.selectedIndex];

            switch (listItem)
            {
            case 0:
                entity.position = placementPoint(*player);
                applyEntityTransform(entity);
                if (state.autoGround)
                {
                    requestGroundSnap(*player, state.selectedIndex);
                }
                showActorEdit(*player);
                break;
            case 1:
                state.followCamera = !state.followCamera;
                if (state.followCamera)
                {
                    player->sendClientMessage(
                        Colour::White(), u("Актор следует за взглядом. Летайте, затем /editor чтобы зафиксировать."));
                }
                else
                {
                    showActorEdit(*player);
                }
                break;
            case 2:
                beginMouseEdit(*player);
                break;
            case 3:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveXY;
                player->sendClientMessage(Colour::White(),
                                          u("Стрелки двигают актора (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 4:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveZ;
                player->sendClientMessage(
                    Colour::White(), u("Вверх/вниз меняют высоту (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 5:
                state.followCamera = false;
                state.keyMode = KeyMode::Rotate;
                player->sendClientMessage(
                    Colour::White(), u("Влево/вправо вращают актора (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 6:
                showKeyStepInput(*player);
                break;
            case 7:
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под актором..."));
                showActorEdit(*player);
                break;
            case 8:
                showDistanceInput(*player);
                break;
            case 9:
                showPositionInput(*player);
                break;
            case 10:
                showAngleInput(*player);
                break;
            case 11:
                showChangeModelInput(*player);
                break;
            case 12:
                showAnimationMenu(*player);
                break;
            case 13: // переключить режим анимации
                entity.animLoop = !entity.animLoop;
                applyEntityAnimation(entity);
                showActorEdit(*player);
                break;
            case 14:
                entity.animLib.clear();
                entity.animName.clear();
                if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
                {
                    actor->clearAnimations();
                }
                showActorEdit(*player);
                break;
            case 15:
                duplicateEntity(*player, state.selectedIndex);
                showEntityEdit(*player);
                break;
            case 16: // перелететь
                state.cameraPosition = entity.position + Vector3(2.0f, 2.0f, 2.0f);
                if (state.cameraObjectId >= 0 && m_objects)
                {
                    if (IObject *camObject = m_objects->get(state.cameraObjectId))
                    {
                        camObject->setPosition(state.cameraPosition);
                    }
                }
                showActorEdit(*player);
                break;
            case 17:
                deleteEntity(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Актор удалён"));
                showMain(*player);
                break;
            default:
                break;
            }
        });
}

void EditorSystem::showVehicleEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (!selectedValid(state))
    {
        showMain(player);
        return;
    }
    const EditorEntity &e = state.entities[state.selectedIndex];
    refreshCheckpointPreview(player); // выбор мог смениться с чекпоинта или на него

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
    body += "Редактировать мышью (прокси-маркер)\n";
    body += "Двигать клавишами (XY)\n";
    body += "Двигать клавишами (высота)\n";
    body += "Вращать клавишами (угол)\n";
    body += fmt::format("Шаг клавиш: {:.2f}\n", state.keyStep);
    body += "Снэп к земле (FindZ)\n";
    body += fmt::format("Дистанция установки: {:.1f}\n", state.placeDistance);
    body += fmt::format("Позиция: {:.1f} {:.1f} {:.1f}\n", e.position.x, e.position.y, e.position.z);
    body += fmt::format("Угол: {:.1f}\n", e.rotation.z);
    body += fmt::format("Сменить модель ({})\n", e.model);
    body += fmt::format("Цвета: {} {}\n", e.colour1, e.colour2);
    body += "Дублировать\n";
    body += "Перелететь к машине\n";
    body += "Удалить";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Машина (модель {})", e.model), body, "Выбрать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            if (response == DialogResponse_Right || !selectedValid(state))
            {
                showMain(*player);
                return;
            }
            EditorEntity &entity = state.entities[state.selectedIndex];

            switch (listItem)
            {
            case 0:
                entity.position = placementPoint(*player);
                applyEntityTransform(entity);
                if (state.autoGround)
                {
                    requestGroundSnap(*player, state.selectedIndex);
                }
                showVehicleEdit(*player);
                break;
            case 1:
                state.followCamera = !state.followCamera;
                if (state.followCamera)
                {
                    player->sendClientMessage(
                        Colour::White(), u("Машина следует за взглядом. Летайте, затем /editor чтобы зафиксировать."));
                }
                else
                {
                    showVehicleEdit(*player);
                }
                break;
            case 2:
                beginMouseEdit(*player);
                break;
            case 3:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveXY;
                player->sendClientMessage(Colour::White(),
                                          u("Стрелки двигают машину (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 4:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveZ;
                player->sendClientMessage(
                    Colour::White(), u("Вверх/вниз меняют высоту (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 5:
                state.followCamera = false;
                state.keyMode = KeyMode::Rotate;
                player->sendClientMessage(
                    Colour::White(), u("Влево/вправо вращают машину (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 6:
                showKeyStepInput(*player);
                break;
            case 7:
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под машиной..."));
                showVehicleEdit(*player);
                break;
            case 8:
                showDistanceInput(*player);
                break;
            case 9:
                showPositionInput(*player);
                break;
            case 10:
                showAngleInput(*player);
                break;
            case 11:
                showChangeModelInput(*player);
                break;
            case 12:
                showVehicleColoursInput(*player);
                break;
            case 13:
                duplicateEntity(*player, state.selectedIndex);
                showEntityEdit(*player);
                break;
            case 14: // перелететь
                state.cameraPosition = entity.position + Vector3(2.0f, 2.0f, 2.0f);
                if (state.cameraObjectId >= 0 && m_objects)
                {
                    if (IObject *camObject = m_objects->get(state.cameraObjectId))
                    {
                        camObject->setPosition(state.cameraPosition);
                    }
                }
                showVehicleEdit(*player);
                break;
            case 15:
                deleteEntity(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Машина удалена"));
                showMain(*player);
                break;
            default:
                break;
            }
        });
}

void EditorSystem::showPickupEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (!selectedValid(state))
    {
        showMain(player);
        return;
    }
    const EditorEntity &e = state.entities[state.selectedIndex];
    refreshCheckpointPreview(player); // выбор мог смениться с чекпоинта или на него

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
    body += "Редактировать мышью (прокси-маркер)\n";
    body += "Двигать клавишами (XY)\n";
    body += "Двигать клавишами (высота)\n";
    body += fmt::format("Шаг клавиш: {:.2f}\n", state.keyStep);
    body += "Снэп к земле (FindZ)\n";
    body += fmt::format("Дистанция установки: {:.1f}\n", state.placeDistance);
    body += fmt::format("Позиция: {:.1f} {:.1f} {:.1f}\n", e.position.x, e.position.y, e.position.z);
    body += fmt::format("Сменить модель ({})\n", e.model);
    body += fmt::format("Тип пикапа: {}\n", e.pickupType);
    body += "Дублировать\n";
    body += "Перелететь к пикапу\n";
    body += "Удалить";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Пикап (модель {})", e.model), body, "Выбрать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            if (response == DialogResponse_Right || !selectedValid(state))
            {
                showMain(*player);
                return;
            }
            EditorEntity &entity = state.entities[state.selectedIndex];

            switch (listItem)
            {
            case 0:
                entity.position = placementPoint(*player);
                applyEntityTransform(entity);
                if (state.autoGround)
                {
                    requestGroundSnap(*player, state.selectedIndex);
                }
                showPickupEdit(*player);
                break;
            case 1:
                state.followCamera = !state.followCamera;
                if (state.followCamera)
                {
                    player->sendClientMessage(
                        Colour::White(), u("Пикап следует за взглядом. Летайте, затем /editor чтобы зафиксировать."));
                }
                else
                {
                    showPickupEdit(*player);
                }
                break;
            case 2:
                beginMouseEdit(*player);
                break;
            case 3:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveXY;
                player->sendClientMessage(Colour::White(),
                                          u("Стрелки двигают пикап (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 4:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveZ;
                player->sendClientMessage(
                    Colour::White(), u("Вверх/вниз меняют высоту (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 5:
                showKeyStepInput(*player);
                break;
            case 6:
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под пикапом..."));
                showPickupEdit(*player);
                break;
            case 7:
                showDistanceInput(*player);
                break;
            case 8:
                showPositionInput(*player);
                break;
            case 9:
                showChangeModelInput(*player);
                break;
            case 10:
                showPickupTypeInput(*player);
                break;
            case 11:
                duplicateEntity(*player, state.selectedIndex);
                showEntityEdit(*player);
                break;
            case 12: // перелететь
                state.cameraPosition = entity.position + Vector3(2.0f, 2.0f, 2.0f);
                if (state.cameraObjectId >= 0 && m_objects)
                {
                    if (IObject *camObject = m_objects->get(state.cameraObjectId))
                    {
                        camObject->setPosition(state.cameraPosition);
                    }
                }
                showPickupEdit(*player);
                break;
            case 13:
                deleteEntity(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Пикап удалён"));
                showMain(*player);
                break;
            default:
                break;
            }
        });
}

void EditorSystem::showCheckpointEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (!selectedValid(state))
    {
        showMain(player);
        return;
    }
    const EditorEntity &e = state.entities[state.selectedIndex];
    refreshCheckpointPreview(player);

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
    body += "Редактировать мышью (прокси-маркер)\n";
    body += "Двигать клавишами (XY)\n";
    body += "Двигать клавишами (высота)\n";
    body += "Менять радиус клавишами\n";
    body += fmt::format("Шаг клавиш: {:.2f}\n", state.keyStep);
    body += "Снэп к земле (FindZ)\n";
    body += fmt::format("Дистанция установки: {:.1f}\n", state.placeDistance);
    body += fmt::format("Позиция: {:.1f} {:.1f} {:.1f}\n", e.position.x, e.position.y, e.position.z);
    body += fmt::format("Радиус: {:.1f}\n", e.radius);
    body += "Дублировать\n";
    body += "Перелететь к чекпоинту\n";
    body += "Удалить";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Чекпоинт (радиус {:.1f})", e.radius), body, "Выбрать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            if (response == DialogResponse_Right || !selectedValid(state))
            {
                showMain(*player);
                return;
            }
            EditorEntity &entity = state.entities[state.selectedIndex];

            switch (listItem)
            {
            case 0:
                entity.position = placementPoint(*player);
                applyEntityTransform(entity);
                refreshCheckpointPreview(*player);
                if (state.autoGround)
                {
                    requestGroundSnap(*player, state.selectedIndex);
                }
                showCheckpointEdit(*player);
                break;
            case 1:
                state.followCamera = !state.followCamera;
                if (state.followCamera)
                {
                    player->sendClientMessage(
                        Colour::White(), u("Чекпоинт следует за взглядом. Летайте, затем /editor чтобы зафиксировать."));
                }
                else
                {
                    showCheckpointEdit(*player);
                }
                break;
            case 2:
                beginMouseEdit(*player);
                break;
            case 3:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveXY;
                player->sendClientMessage(
                    Colour::White(), u("Стрелки двигают чекпоинт (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 4:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveZ;
                player->sendClientMessage(
                    Colour::White(), u("Вверх/вниз меняют высоту (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 5:
                state.followCamera = false;
                state.keyMode = KeyMode::Rotate; // для чекпоинта вращение = радиус
                player->sendClientMessage(
                    Colour::White(), u("Влево/вправо меняют радиус (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 6:
                showKeyStepInput(*player);
                break;
            case 7:
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под чекпоинтом..."));
                showCheckpointEdit(*player);
                break;
            case 8:
                showDistanceInput(*player);
                break;
            case 9:
                showPositionInput(*player);
                break;
            case 10:
                showRadiusInput(*player);
                break;
            case 11:
                duplicateEntity(*player, state.selectedIndex);
                showEntityEdit(*player);
                break;
            case 12: // перелететь
                state.cameraPosition = entity.position + Vector3(2.0f, 2.0f, 2.0f);
                if (state.cameraObjectId >= 0 && m_objects)
                {
                    if (IObject *camObject = m_objects->get(state.cameraObjectId))
                    {
                        camObject->setPosition(state.cameraPosition);
                    }
                }
                showCheckpointEdit(*player);
                break;
            case 13:
                deleteEntity(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Чекпоинт удалён"));
                showMain(*player);
                break;
            default:
                break;
            }
        });
}

void EditorSystem::showRadiusInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Радиус чекпоинта",
                   fmt::format("Введите радиус ({:.1f}-{:.1f})", CheckpointService::MIN_RADIUS,
                               CheckpointService::MAX_RADIUS),
                   "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            if (response == DialogResponse_Left && selectedValid(state))
            {
                float radius = 0.0f;
                if (parseFloat(text.to_string(), radius))
                {
                    state.entities[state.selectedIndex].radius =
                        std::clamp(radius, CheckpointService::MIN_RADIUS, CheckpointService::MAX_RADIUS);
                    applyEntityTransform(state.entities[state.selectedIndex]);
                    refreshCheckpointPreview(*player);
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите корректное число"));
                }
            }

            showEntityEdit(*player);
        });
}

void EditorSystem::showPickupModelInput(IPlayer &player)
{
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Создать пикап", "Введите ID модели пикапа (напр. 1212 — деньги, 1240 — сердце)",
                   "Создать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Right)
            {
                showMain(*player);
                return;
            }

            if (value < 0 || value > MAX_OBJECT_MODEL)
            {
                player->sendClientMessage(Colour::White(),
                                          u(fmt::format("Введите ID модели от 0 до {}", MAX_OBJECT_MODEL)));
                showPickupModelInput(*player);
                return;
            }

            createPickupEntity(*player, static_cast<int>(value));
            showEntityEdit(*player);
        });
}

void EditorSystem::showPickupTypeInput(IPlayer &player)
{
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Тип пикапа",
                   u(fmt::format("Введите тип поведения (0-{}). Частые: 1 — статичный, 2 — исчезает и респавнится, "
                                 "14 — подбор из машины",
                                 MAX_PICKUP_TYPE)),
                   "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            if (response == DialogResponse_Left && selectedValid(state))
            {
                if (value < 0 || value > MAX_PICKUP_TYPE)
                {
                    player->sendClientMessage(Colour::White(),
                                              u(fmt::format("Введите тип от 0 до {}", MAX_PICKUP_TYPE)));
                    showPickupTypeInput(*player);
                    return;
                }

                const int type = static_cast<int>(value);
                EditorEntity &entity = state.entities[state.selectedIndex];
                entity.pickupType = type;
                if (IPickup *pickup = m_pickups ? m_pickups->get(entity.entityId) : nullptr)
                {
                    pickup->setType(static_cast<PickupType>(type));
                }
            }

            showEntityEdit(*player);
        });
}

void EditorSystem::showObjectModelInput(IPlayer &player)
{
    m_dialogService.showNumberInput(
        player, makeDialog(DialogStyle_INPUT, "Создать объект", "Введите ID модели объекта", "Создать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Right)
            {
                showMain(*player);
                return;
            }

            if (value < 0 || value > MAX_OBJECT_MODEL)
            {
                player->sendClientMessage(Colour::White(),
                                          u(fmt::format("Введите ID модели от 0 до {}", MAX_OBJECT_MODEL)));
                showObjectModelInput(*player);
                return;
            }

            createObjectEntity(*player, static_cast<int>(value));
            showEntityEdit(*player);
        });
}

void EditorSystem::showActorSkinInput(IPlayer &player)
{
    m_dialogService.showNumberInput(
        player, makeDialog(DialogStyle_INPUT, "Создать актора", "Введите ID скина актора", "Создать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Right)
            {
                showMain(*player);
                return;
            }

            if (value < 0 || value > MAX_ACTOR_SKIN)
            {
                player->sendClientMessage(Colour::White(),
                                          u(fmt::format("Введите ID скина от 0 до {}", MAX_ACTOR_SKIN)));
                showActorSkinInput(*player);
                return;
            }

            createActorEntity(*player, static_cast<int>(value));
            showEntityEdit(*player);
        });
}

void EditorSystem::showVehicleModelInput(IPlayer &player)
{
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Создать машину",
                   fmt::format("Введите ID модели машины ({}-{})", MIN_VEHICLE_MODEL, MAX_VEHICLE_MODEL), "Создать",
                   "Назад"),
        [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Right)
            {
                showMain(*player);
                return;
            }

            if (value < MIN_VEHICLE_MODEL || value > MAX_VEHICLE_MODEL)
            {
                player->sendClientMessage(
                    Colour::White(), u(fmt::format("Введите ID модели от {} до {}", MIN_VEHICLE_MODEL, MAX_VEHICLE_MODEL)));
                showVehicleModelInput(*player);
                return;
            }

            createVehicleEntity(*player, static_cast<int>(value));
            showEntityEdit(*player);
        });
}

void EditorSystem::showVehicleColoursInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Цвета машины", "Введите два ID цвета: первый второй (-1 — случайный)", "OK",
                   "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            if (response == DialogResponse_Left && selectedValid(state))
            {
                EditorEntity &entity = state.entities[state.selectedIndex];

                int colour1 = 0;
                int colour2 = 0;
                std::istringstream ss(text.to_string());
                if (!(ss >> colour1 >> colour2) || colour1 < -1 || colour1 > MAX_VEHICLE_COLOUR || colour2 < -1 ||
                    colour2 > MAX_VEHICLE_COLOUR)
                {
                    player->sendClientMessage(
                        Colour::White(), u(fmt::format("Введите два числа от -1 до {}", MAX_VEHICLE_COLOUR)));
                    showVehicleColoursInput(*player);
                    return;
                }

                entity.colour1 = colour1;
                entity.colour2 = colour2;
                if (IVehicle *vehicle = m_vehicleService.get(entity.entityId))
                {
                    vehicle->setColour(colour1, colour2);
                }
            }

            showEntityEdit(*player);
        });
}

void EditorSystem::showChangeModelInput(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (!selectedValid(state))
    {
        showMain(player);
        return;
    }
    const EntityType type = state.entities[state.selectedIndex].type;
    const bool isActor = type == EntityType::Actor;

    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, isActor ? "Сменить скин" : "Сменить модель",
                   isActor ? "Введите новый ID скина" : "Введите новый ID модели", "OK", "Назад"),
        [this, playerId = player.getID(), type](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            if (response == DialogResponse_Left && selectedValid(state))
            {
                EditorEntity &entity = state.entities[state.selectedIndex];

                int minModel = 0;
                int maxModel = MAX_OBJECT_MODEL;
                if (type == EntityType::Actor)
                {
                    maxModel = MAX_ACTOR_SKIN;
                }
                else if (type == EntityType::Vehicle)
                {
                    minModel = MIN_VEHICLE_MODEL;
                    maxModel = MAX_VEHICLE_MODEL;
                }

                if (value < minModel || value > maxModel)
                {
                    player->sendClientMessage(Colour::White(),
                                              u(fmt::format("Введите ID от {} до {}", minModel, maxModel)));
                    showChangeModelInput(*player);
                    return;
                }

                const int model = static_cast<int>(value);

                if (entity.type == EntityType::Object)
                {
                    entity.model = model;
                    if (IObject *object = m_objects ? m_objects->get(entity.entityId) : nullptr)
                    {
                        object->setModel(model);
                    }
                }
                else if (entity.type == EntityType::Actor)
                {
                    entity.model = model;
                    if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
                    {
                        actor->setSkin(model);
                        applyEntityAnimation(entity); // смена скина сбрасывает анимацию на клиенте
                    }
                }
                else if (entity.type == EntityType::Vehicle)
                {
                    // У машин модель не меняется на месте — пересоздаём с тем же трансформом.
                    const int oldModel = entity.model;
                    const int oldId = entity.entityId;
                    entity.model = model;
                    if (IVehicle *vehicle = spawnVehicle(entity))
                    {
                        entity.entityId = vehicle->getID();
                        m_vehicleService.setEditBypass(entity.entityId, true);
                        m_vehicleService.destroy(oldId);
                    }
                    else
                    {
                        entity.model = oldModel;
                        player->sendClientMessage(Colour::White(), u("Не удалось пересоздать машину"));
                    }
                }
                else
                {
                    entity.model = model;
                    if (IPickup *pickup = m_pickups ? m_pickups->get(entity.entityId) : nullptr)
                    {
                        pickup->setModel(model);
                    }
                }
            }

            showEntityEdit(*player);
        });
}

void EditorSystem::showPositionInput(IPlayer &player)
{
    m_dialogService.show(player, makeDialog(DialogStyle_INPUT, "Позиция", "Введите координаты: X Y Z", "OK", "Назад"),
                         [this, playerId = player.getID()](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             EditorState &state = m_state[playerId];

                             if (response == DialogResponse_Left && selectedValid(state))
                             {
                                 Vector3 pos;
                                 if (parseVec3(text.to_string(), pos))
                                 {
                                     state.entities[state.selectedIndex].position = pos;
                                     applyEntityTransform(state.entities[state.selectedIndex]);
                                 }
                                 else
                                 {
                                     player->sendClientMessage(Colour::White(), u("Введите три числа: X Y Z"));
                                 }
                             }

                             showEntityEdit(*player);
                         });
}

void EditorSystem::showRotationInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Поворот", "Введите углы в градусах: X Y Z", "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];

            if (response == DialogResponse_Left && selectedValid(state))
            {
                Vector3 rotation;
                if (parseVec3(text.to_string(), rotation))
                {
                    state.entities[state.selectedIndex].rotation = rotation;
                    applyEntityTransform(state.entities[state.selectedIndex]);
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите три числа: X Y Z"));
                }
            }

            showEntityEdit(*player);
        });
}

void EditorSystem::showAngleInput(IPlayer &player)
{
    m_dialogService.show(player, makeDialog(DialogStyle_INPUT, "Угол", "Введите угол поворота в градусах", "OK", "Назад"),
                         [this, playerId = player.getID()](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             EditorState &state = m_state[playerId];

                             if (response == DialogResponse_Left && selectedValid(state))
                             {
                                 float angle = 0.0f;
                                 if (parseFloat(text.to_string(), angle))
                                 {
                                     state.entities[state.selectedIndex].rotation.z = wrapDegrees(angle);
                                     applyEntityTransform(state.entities[state.selectedIndex]);
                                 }
                                 else
                                 {
                                     player->sendClientMessage(Colour::White(), u("Введите корректный угол"));
                                 }
                             }

                             showEntityEdit(*player);
                         });
}

void EditorSystem::showKeyStepInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Шаг клавиш",
                   fmt::format("Введите шаг перемещения в метрах за тик ({}-{})", KEY_STEP_MIN, KEY_STEP_MAX), "OK",
                   "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                float step = 0.0f;
                if (parseFloat(text.to_string(), step))
                {
                    m_state[playerId].keyStep = std::clamp(step, KEY_STEP_MIN, KEY_STEP_MAX);
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите корректное число"));
                }
            }

            showEntityEdit(*player);
        });
}

void EditorSystem::showDistanceInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Дистанция установки", "Введите дистанцию (1-100)", "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                float dist = 0.0f;
                if (parseFloat(text.to_string(), dist))
                {
                    m_state[playerId].placeDistance = std::clamp(dist, 1.0f, 100.0f);
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите корректное число"));
                }
            }

            showEntityEdit(*player);
        });
}

void EditorSystem::showInteriorInput(IPlayer &player)
{
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Интерьер",
                   fmt::format("Введите ID интерьера (0-{}). 0 — внешний мир.", MAX_INTERIOR_ID), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                if (value >= 0 && value <= MAX_INTERIOR_ID)
                {
                    m_locationService.setInterior(*player, static_cast<unsigned>(value));
                }
                else
                {
                    player->sendClientMessage(Colour::White(),
                                              u(fmt::format("Введите ID от 0 до {}", MAX_INTERIOR_ID)));
                }
            }

            showMain(*player);
        });
}

void EditorSystem::showInteriorList(IPlayer &player, int page)
{
    constexpr int pageCount = (INTERIOR_SPOT_COUNT + INTERIOR_PAGE_SIZE - 1) / INTERIOR_PAGE_SIZE;
    page = std::clamp(page, 0, pageCount - 1);
    const int first = page * INTERIOR_PAGE_SIZE;
    const int count = std::min(INTERIOR_PAGE_SIZE, INTERIOR_SPOT_COUNT - first);

    std::string body;
    for (int i = first; i < first + count; ++i)
    {
        body += fmt::format("{}\tID {}\n", INTERIOR_SPOTS[i].name, INTERIOR_SPOTS[i].interior);
    }
    body += fmt::format("» Следующая страница ({}/{})", page + 1, pageCount);

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_LIST, fmt::format("Телепорт по интерьерам {}/{}", page + 1, pageCount), body,
                   "Телепорт", "Назад"),
        [this, playerId = player.getID(), page, first, count](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response != DialogResponse_Left || listItem < 0 || listItem > count)
            {
                showMain(*player);
                return;
            }
            if (listItem == count) // последняя строка — следующая страница (по кругу)
            {
                constexpr int pageCount = (INTERIOR_SPOT_COUNT + INTERIOR_PAGE_SIZE - 1) / INTERIOR_PAGE_SIZE;
                showInteriorList(*player, (page + 1) % pageCount);
                return;
            }

            const InteriorSpot &spot = INTERIOR_SPOTS[first + listItem];
            const Vector3 position(spot.x, spot.y, spot.z);

            EditorState &state = m_state[playerId];
            // Переносим и тело, и камеру: интерьеры лежат в «виртуальных комнатах»
            // на высоте ~1000, лететь туда камерой бессмысленно.
            m_locationService.setInterior(*player, static_cast<unsigned>(spot.interior));
            m_locationService.teleport(*player, position);
            state.returnPosition = position;
            state.cameraPosition = position + Vector3(0.0f, 0.0f, 1.0f);
            if (state.cameraObjectId >= 0 && m_objects)
            {
                if (IObject *camObject = m_objects->get(state.cameraObjectId))
                {
                    camObject->setPosition(state.cameraPosition);
                }
            }

            player->sendClientMessage(
                Colour::White(), u(fmt::format("Телепорт: {} (интерьер {}). /editor — меню.", spot.name, spot.interior)));
        });
}

void EditorSystem::showCameraSpeedInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Скорость камеры",
                   fmt::format("Введите скорость полёта ({}-{})", CAMERA_SPEED_MIN, CAMERA_SPEED_MAX), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                float speed = 0.0f;
                if (parseFloat(text.to_string(), speed))
                {
                    m_state[playerId].cameraSpeed = std::clamp(speed, CAMERA_SPEED_MIN, CAMERA_SPEED_MAX);
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите корректное число"));
                }
            }

            showMain(*player);
        });
}

void EditorSystem::showAnimationMenu(IPlayer &player)
{
    std::string body;
    for (const AnimPreset &preset : ANIM_PRESETS)
    {
        body += fmt::format("{}\t{}:{}\n", preset.label, preset.lib, preset.name);
    }
    body += "Ввести вручную (библиотека и название)";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Анимация NPC", body, "Выбрать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];
            if (response != DialogResponse_Left || !selectedValid(state))
            {
                showEntityEdit(*player);
                return;
            }

            if (listItem >= 0 && listItem < ANIM_PRESET_COUNT)
            {
                const AnimPreset &preset = ANIM_PRESETS[listItem];
                setActorAnimation(*player, state.entities[state.selectedIndex], preset.lib, preset.name,
                                  state.entities[state.selectedIndex].animLoop);
                showEntityEdit(*player);
                return;
            }

            showAnimLibInput(*player);
        });
}

void EditorSystem::showAnimLibInput(IPlayer &player)
{
    m_dialogService.show(player,
                         makeDialog(DialogStyle_INPUT, "Анимация — библиотека",
                                    "Введите библиотеку анимации (например, DANCING)", "Далее", "Назад"),
                         [this, playerId = player.getID()](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 showAnimationMenu(*player);
                                 return;
                             }

                             showAnimNameInput(*player, text.to_string());
                         });
}

void EditorSystem::showAnimNameInput(IPlayer &player, std::string animLib)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Анимация — название", "Введите название анимации (например, dnce_M_b)",
                   "Применить", "Назад"),
        [this, playerId = player.getID(), animLib = std::move(animLib)](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            EditorState &state = m_state[playerId];

            if (response == DialogResponse_Right || !selectedValid(state))
            {
                showAnimationMenu(*player);
                return;
            }

            EditorEntity &entity = state.entities[state.selectedIndex];
            if (!setActorAnimation(*player, entity, animLib, text.to_string(), entity.animLoop))
            {
                showAnimNameInput(*player, animLib); // имя не нашлось — дать поправить
                return;
            }

            showEntityEdit(*player);
        });
}

void EditorSystem::showSaveNameInput(IPlayer &player)
{
    m_dialogService.show(player,
                         makeDialog(DialogStyle_INPUT, "Сохранить карту", "Введите имя файла", "Сохранить", "Назад"),
                         [this, playerId = player.getID()](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 showMain(*player);
                                 return;
                             }

                             const std::string name = text.to_string();
                             if (!Utils::isValidPresetName(name))
                             {
                                 player->sendClientMessage(Colour::White(),
                                                           u("Недопустимое имя файла (разрешены A-Za-z0-9, _, -)"));
                                 showSaveNameInput(*player);
                                 return;
                             }

                             saveToFileAsync(*player, name); // запись на воркере, сообщение придёт из колбэка
                             showMain(*player);
                         });
}

void EditorSystem::showClearConfirm(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (state.entities.empty())
    {
        showMain(player);
        return;
    }

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_MSGBOX, "Очистить сцену",
                   fmt::format("Удалить все сущности ({})? Несохранённое будет потеряно.", state.entities.size()),
                   "Удалить", "Отмена"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                clearScene(*player);
                player->sendClientMessage(Colour::White(), u("Сцена очищена"));
            }
            showMain(*player);
        });
}

void EditorSystem::showObjectList(IPlayer &player)
{
    EditorState &state = stateOf(player);

    std::vector<int> mapping; // строка списка -> индекс в entities
    std::string body;
    for (size_t i = 0; i < state.entities.size(); ++i)
    {
        const EditorEntity &e = state.entities[i];
        if (e.type != EntityType::Object)
        {
            continue;
        }
        body +=
            fmt::format("#{}\tмодель {}\t{:.1f} {:.1f} {:.1f}\n", i, e.model, e.position.x, e.position.y, e.position.z);
        mapping.push_back(static_cast<int>(i));
    }
    if (body.empty())
    {
        body = "Список пуст";
    }

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Объекты на сцене", body, "Выбрать", "Назад"),
                         [this, playerId = player.getID(), mapping = std::move(mapping)](DialogResponse response,
                                                                                         int listItem, StringView)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response != DialogResponse_Left || listItem < 0 || listItem >= (int)mapping.size())
                             {
                                 showMain(*player);
                                 return;
                             }

                             m_state[playerId].selectedIndex = mapping[listItem];
                             showObjectEdit(*player);
                         });
}

void EditorSystem::showActorList(IPlayer &player)
{
    EditorState &state = stateOf(player);

    std::vector<int> mapping;
    std::string body;
    for (size_t i = 0; i < state.entities.size(); ++i)
    {
        const EditorEntity &e = state.entities[i];
        if (e.type != EntityType::Actor)
        {
            continue;
        }
        body +=
            fmt::format("#{}\tскин {}\t{:.1f} {:.1f} {:.1f}\n", i, e.model, e.position.x, e.position.y, e.position.z);
        mapping.push_back(static_cast<int>(i));
    }
    if (body.empty())
    {
        body = "Список пуст";
    }

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Акторы на сцене", body, "Выбрать", "Назад"),
                         [this, playerId = player.getID(), mapping = std::move(mapping)](DialogResponse response,
                                                                                         int listItem, StringView)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response != DialogResponse_Left || listItem < 0 || listItem >= (int)mapping.size())
                             {
                                 showMain(*player);
                                 return;
                             }

                             m_state[playerId].selectedIndex = mapping[listItem];
                             showActorEdit(*player);
                         });
}

void EditorSystem::showVehicleList(IPlayer &player)
{
    EditorState &state = stateOf(player);

    std::vector<int> mapping;
    std::string body;
    for (size_t i = 0; i < state.entities.size(); ++i)
    {
        const EditorEntity &e = state.entities[i];
        if (e.type != EntityType::Vehicle)
        {
            continue;
        }
        body +=
            fmt::format("#{}\tмодель {}\t{:.1f} {:.1f} {:.1f}\n", i, e.model, e.position.x, e.position.y, e.position.z);
        mapping.push_back(static_cast<int>(i));
    }
    if (body.empty())
    {
        body = "Список пуст";
    }

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Машины на сцене", body, "Выбрать", "Назад"),
                         [this, playerId = player.getID(), mapping = std::move(mapping)](DialogResponse response,
                                                                                         int listItem, StringView)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response != DialogResponse_Left || listItem < 0 || listItem >= (int)mapping.size())
                             {
                                 showMain(*player);
                                 return;
                             }

                             m_state[playerId].selectedIndex = mapping[listItem];
                             showVehicleEdit(*player);
                         });
}

void EditorSystem::showPickupList(IPlayer &player)
{
    EditorState &state = stateOf(player);

    std::vector<int> mapping;
    std::string body;
    for (size_t i = 0; i < state.entities.size(); ++i)
    {
        const EditorEntity &e = state.entities[i];
        if (e.type != EntityType::Pickup)
        {
            continue;
        }
        body += fmt::format("#{}\tмодель {}\tтип {}\t{:.1f} {:.1f} {:.1f}\n", i, e.model, e.pickupType, e.position.x,
                            e.position.y, e.position.z);
        mapping.push_back(static_cast<int>(i));
    }
    if (body.empty())
    {
        body = "Список пуст";
    }

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Пикапы на сцене", body, "Выбрать", "Назад"),
                         [this, playerId = player.getID(), mapping = std::move(mapping)](DialogResponse response,
                                                                                         int listItem, StringView)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response != DialogResponse_Left || listItem < 0 || listItem >= (int)mapping.size())
                             {
                                 showMain(*player);
                                 return;
                             }

                             m_state[playerId].selectedIndex = mapping[listItem];
                             showPickupEdit(*player);
                         });
}

void EditorSystem::showCheckpointList(IPlayer &player)
{
    EditorState &state = stateOf(player);

    std::vector<int> mapping;
    std::string body;
    for (size_t i = 0; i < state.entities.size(); ++i)
    {
        const EditorEntity &e = state.entities[i];
        if (e.type != EntityType::Checkpoint)
        {
            continue;
        }
        body += fmt::format("#{}\tрадиус {:.1f}\t{:.1f} {:.1f} {:.1f}\n", i, e.radius, e.position.x, e.position.y,
                            e.position.z);
        mapping.push_back(static_cast<int>(i));
    }
    if (body.empty())
    {
        body = "Список пуст";
    }

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Чекпоинты на сцене", body, "Выбрать", "Назад"),
                         [this, playerId = player.getID(), mapping = std::move(mapping)](DialogResponse response,
                                                                                         int listItem, StringView)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response != DialogResponse_Left || listItem < 0 || listItem >= (int)mapping.size())
                             {
                                 showMain(*player);
                                 return;
                             }

                             m_state[playerId].selectedIndex = mapping[listItem];
                             showCheckpointEdit(*player);
                         });
}

void EditorSystem::showLoadList(IPlayer &player, std::vector<std::string> files)
{
    std::string body;
    for (const std::string &name : files)
    {
        body += name + "\n";
    }
    if (body.empty())
    {
        body = "Нет сохранённых карт";
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Загрузить карту", body, "Выбрать", "Назад"),
        [this, playerId = player.getID(), files = std::move(files)](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response != DialogResponse_Left || listItem < 0 || listItem >= (int)files.size())
            {
                showMain(*player);
                return;
            }

            showLoadModeChoice(*player, files[listItem]);
        });
}

void EditorSystem::showLoadModeChoice(IPlayer &player, std::string fileName)
{
    std::string body;
    body += "Заменить сцену (текущие сущности удалить)\n";
    body += "Добавить к текущей сцене";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Загрузка: {}", fileName), body, "Загрузить", "Назад"),
        [this, playerId = player.getID(), fileName = std::move(fileName)](DialogResponse response, int listItem,
                                                                          StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response != DialogResponse_Left || listItem < 0 || listItem > 1)
            {
                listMapFilesAsync(*player);
                return;
            }

            // Чтение на воркере; очистка сцены при замене — только после
            // успешного чтения (в колбэке), чтобы битый файл не стёр работу.
            loadFromFileAsync(*player, fileName, listItem == 0);
        });
}

// ------------------------------------------------------------------ файлы
// Диск (запись, чтение, листинг каталога) — на воркерах тредпула; сериализация,
// парсинг и создание сущностей — на главном потоке (SDK не потокобезопасен).

std::string EditorSystem::serializeScene(const EditorState &state) const
{
    std::string out;
    for (const EditorEntity &e : state.entities)
    {
        if (e.type == EntityType::Object)
        {
            out += fmt::format("object {} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f}\n", e.model, e.position.x,
                               e.position.y, e.position.z, e.rotation.x, e.rotation.y, e.rotation.z);
        }
        else if (e.type == EntityType::Actor)
        {
            const std::string lib = e.animLib.empty() ? "-" : e.animLib;
            const std::string anim = e.animName.empty() ? "-" : e.animName;
            // Последний токен — режим анимации; старые файлы без него читаются как loop.
            out += fmt::format("actor {} {:.4f} {:.4f} {:.4f} {:.4f} {} {} {}\n", e.model, e.position.x, e.position.y,
                               e.position.z, e.rotation.z, lib, anim, e.animLoop ? "loop" : "freeze");
        }
        else if (e.type == EntityType::Vehicle)
        {
            out += fmt::format("vehicle {} {:.4f} {:.4f} {:.4f} {:.4f} {} {}\n", e.model, e.position.x, e.position.y,
                               e.position.z, e.rotation.z, e.colour1, e.colour2);
        }
        else if (e.type == EntityType::Pickup)
        {
            out += fmt::format("pickup {} {} {:.4f} {:.4f} {:.4f}\n", e.model, e.pickupType, e.position.x,
                               e.position.y, e.position.z);
        }
        else
        {
            out += fmt::format("checkpoint {:.4f} {:.4f} {:.4f} {:.2f}\n", e.position.x, e.position.y, e.position.z,
                               e.radius);
        }
    }
    return out;
}

void EditorSystem::saveToFileAsync(IPlayer &player, const std::string &name)
{
    if (asyncOpBusy(player))
    {
        return;
    }
    const std::string path = MAPS_DIR + "/" + name + ".txt";

    ThreadPool::Task<bool> task;
    task.func = [path, content = serializeScene(stateOf(player))]()
    {
        std::error_code ec;
        std::filesystem::create_directories(MAPS_DIR, ec);
        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            throw std::runtime_error("не удалось открыть файл " + path);
        }
        out << content;
        if (!out.good())
        {
            throw std::runtime_error("ошибка записи " + path);
        }
        return true;
    };
    task.callback = [this, playerId = player.getID(), path](bool)
    {
        m_state[playerId].asyncOpPending = false;
        if (IPlayer *player = editorPlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Карта сохранена: " + path));
        }
    };
    task.errorCallback = [this, playerId = player.getID()](const std::string &error)
    {
        m_state[playerId].asyncOpPending = false;
        if (IPlayer *player = editorPlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Ошибка сохранения: " + error));
        }
    };
    stateOf(player).asyncOpPending = true;
    ThreadPool::addTask(std::move(task));
}

void EditorSystem::loadFromFileAsync(IPlayer &player, const std::string &name, bool replace)
{
    if (asyncOpBusy(player))
    {
        return;
    }
    // Ре-санитизация имени перед построением пути чтения (defense-in-depth: даже
    // при выборе из листинга имя не должно вырваться из MAPS_DIR).
    if (!Utils::isValidPresetName(name))
    {
        player.sendClientMessage(Colour::White(), u("Недопустимое имя файла"));
        showMain(player);
        return;
    }
    const std::string path = MAPS_DIR + "/" + name + ".txt";

    ThreadPool::Task<std::string> task;
    task.func = [path]()
    {
        std::ifstream in(path);
        if (!in)
        {
            throw std::runtime_error("не удалось открыть файл " + path);
        }
        std::ostringstream content;
        content << in.rdbuf();
        return content.str();
    };
    task.callback = [this, playerId = player.getID(), name, replace](std::string content)
    {
        m_state[playerId].asyncOpPending = false; // снимаем гард до мутаций сцены в этом же колбэке
        IPlayer *player = editorPlayer(playerId);
        if (!player)
        {
            return; // вышел из редактора, пока читали — сцену не трогаем
        }

        if (replace)
        {
            clearScene(*player); // только после успешного чтения файла
        }
        const std::size_t loaded = loadFromContent(*player, content);
        if (loaded > 0)
        {
            player->sendClientMessage(Colour::White(),
                                      u(fmt::format("Карта загружена: {} (сущностей: {})", name, loaded)));
        }
        else
        {
            player->sendClientMessage(Colour::White(), u("Ошибка загрузки: в файле нет валидных сущностей"));
        }
        showMain(*player);
    };
    task.errorCallback = [this, playerId = player.getID()](const std::string &error)
    {
        m_state[playerId].asyncOpPending = false;
        if (IPlayer *player = editorPlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Ошибка загрузки: " + error));
            showMain(*player);
        }
    };
    stateOf(player).asyncOpPending = true;
    ThreadPool::addTask(std::move(task));
}

void EditorSystem::listMapFilesAsync(IPlayer &player)
{
    if (asyncOpBusy(player))
    {
        return;
    }
    ThreadPool::Task<std::vector<std::string>> task;
    task.func = []()
    {
        std::vector<std::string> result;
        std::error_code ec;
        if (!std::filesystem::exists(MAPS_DIR, ec))
        {
            return result;
        }
        for (const auto &entry : std::filesystem::directory_iterator(MAPS_DIR, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".txt")
            {
                result.push_back(entry.path().stem().string());
            }
        }
        return result;
    };
    task.callback = [this, playerId = player.getID()](std::vector<std::string> files)
    {
        m_state[playerId].asyncOpPending = false;
        if (IPlayer *player = editorPlayer(playerId))
        {
            showLoadList(*player, std::move(files));
        }
    };
    task.errorCallback = [this, playerId = player.getID()](const std::string &) { m_state[playerId].asyncOpPending = false; };
    stateOf(player).asyncOpPending = true;
    ThreadPool::addTask(std::move(task));
}

std::size_t EditorSystem::loadFromContent(IPlayer &player, const std::string &content)
{
    EditorState &state = stateOf(player);
    std::size_t loaded = 0;

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;

        if (kind == "object")
        {
            int model = 0;
            Vector3 pos, rot;
            if (!(ss >> model >> pos.x >> pos.y >> pos.z >> rot.x >> rot.y >> rot.z))
            {
                continue;
            }
            IObject *object = m_objects ? m_objects->create(model, pos, rot) : nullptr;
            if (!object)
            {
                continue;
            }
            EditorEntity entity;
            entity.type = EntityType::Object;
            entity.entityId = object->getID();
            entity.model = model;
            entity.position = pos;
            entity.rotation = rot;
            state.entities.push_back(entity);
            ++loaded;
        }
        else if (kind == "actor")
        {
            int skin = 0;
            Vector3 pos;
            float angle = 0.0f;
            std::string lib, anim;
            if (!(ss >> skin >> pos.x >> pos.y >> pos.z >> angle >> lib >> anim))
            {
                continue;
            }
            std::string mode; // опциональный токен (старые файлы без него — loop)
            ss >> mode;

            IActor *actor = m_actors ? m_actors->create(skin, pos, angle) : nullptr;
            if (!actor)
            {
                continue;
            }
            EditorEntity entity;
            entity.type = EntityType::Actor;
            entity.entityId = actor->getID();
            entity.model = skin;
            entity.position = pos;
            entity.rotation = Vector3(0.0f, 0.0f, angle);
            entity.animLoop = mode != "freeze";
            if (lib != "-" && anim != "-")
            {
                entity.animLib = lib;
                entity.animName = anim;
            }
            state.entities.push_back(entity);
            applyEntityAnimation(state.entities.back());
            ++loaded;
        }
        else if (kind == "vehicle")
        {
            EditorEntity entity;
            entity.type = EntityType::Vehicle;
            float angle = 0.0f;
            if (!(ss >> entity.model >> entity.position.x >> entity.position.y >> entity.position.z >> angle >>
                  entity.colour1 >> entity.colour2))
            {
                continue;
            }
            entity.rotation = Vector3(0.0f, 0.0f, angle);

            IVehicle *vehicle = spawnVehicle(entity);
            if (!vehicle)
            {
                continue;
            }
            entity.entityId = vehicle->getID();
            m_vehicleService.setEditBypass(entity.entityId, true);
            state.entities.push_back(entity);
            ++loaded;
        }
        else if (kind == "pickup")
        {
            EditorEntity entity;
            entity.type = EntityType::Pickup;
            if (!(ss >> entity.model >> entity.pickupType >> entity.position.x >> entity.position.y >>
                  entity.position.z))
            {
                continue;
            }
            entity.pickupType = std::clamp(entity.pickupType, 0, MAX_PICKUP_TYPE);

            IPickup *pickup = m_pickups ? m_pickups->create(entity.model, static_cast<PickupType>(entity.pickupType),
                                                            entity.position, 0, false)
                                        : nullptr;
            if (!pickup)
            {
                continue;
            }
            entity.entityId = pickup->getID();
            state.entities.push_back(entity);
            ++loaded;
        }
        else if (kind == "checkpoint")
        {
            EditorEntity entity;
            entity.type = EntityType::Checkpoint;
            if (!(ss >> entity.position.x >> entity.position.y >> entity.position.z >> entity.radius))
            {
                continue;
            }
            entity.radius = std::clamp(entity.radius, CheckpointService::MIN_RADIUS, CheckpointService::MAX_RADIUS);
            entity.entityId = m_checkpointService.add(entity.position, entity.radius, nullptr);
            if (entity.entityId < 0)
            {
                continue;
            }
            state.entities.push_back(entity);
            ++loaded;
        }
    }

    return loaded;
}
