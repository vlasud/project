#include "Systems/Core/EditorSystem/EditorSystem.h"

#include "Utils/Encoding/Encoding.h"
#include "anim.hpp"
#include "component.hpp"
#include "glm/geometric.hpp"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <sstream>

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

// Зонд FindZ читает позицию стоящего игрока, а это его туловище (~центр педа),
// не ноги: прочитанный Z = уровень земли + PED_ORIGIN_HEIGHT. Объект ставим на
// саму землю, актора — на ту же высоту туловища (его origin такой же), машину —
// чуть выше земли, дальше её усадит клиентская физика.
constexpr float PED_ORIGIN_HEIGHT = 1.0f;     // от ног стоящего педа до его origin
constexpr float VEHICLE_GROUND_OFFSET = 0.5f; // полколеса над землёй
constexpr float PICKUP_GROUND_OFFSET = 0.5f;  // пикап чуть над землёй, чтобы не утонул в текстуре

// Высота, с которой клиент ищет землю (выше самой высокой точки карты ~ г. Чилиад).
constexpr float GROUND_PROBE_Z = 1500.0f;
constexpr int GROUND_PROBE_WAIT = 10;    // тиков ожидания ответа клиента перед чтением Z
constexpr float PROBE_PARK_Z = -1000.0f; // куда временно прячем сущность, чтобы FindZ не попал в неё

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

bool parseInt(StringView text, int &out)
{
    const char *begin = text.data();
    const char *end = begin + text.size();
    while (begin < end && *begin == ' ')
    {
        ++begin;
    }
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc();
}

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
      m_locationService(serviceRegister.getService<PlayerLocationService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);

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
                         });
}

void EditorSystem::initialize(IComponentList *components)
{
    m_objects = components->queryComponent<IObjectsComponent>();
    m_actors = components->queryComponent<IActorsComponent>();
    m_vehicles = components->queryComponent<IVehiclesComponent>();
    m_pickups = components->queryComponent<IPickupsComponent>();
}

EditorSystem::EditorState &EditorSystem::stateOf(const IPlayer &player)
{
    return m_state[player.getID()];
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

    if (state.cameraObjectId >= 0 && m_objects)
    {
        m_objects->release(state.cameraObjectId);
    }
    state.cameraObjectId = -1;
    state.enabled = false;
    state.followCamera = false;
    state.keyMode = KeyMode::Camera;
    state.selectedIndex = -1;

    // Возвращаем тело в точку входа: зонды и слепой бег растаскали его по карте.
    m_locationService.teleport(player, state.returnPosition);
    m_locationService.setBypass(player.getID(), false);
    player.setCameraBehind();
    player.sendClientMessage(Colour::White(), u("Редактор выключен. Расставленные сущности остались на сцене."));
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

void EditorSystem::createActorEntity(IPlayer &player, int skin)
{
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

IVehicle *EditorSystem::spawnVehicle(const EditorEntity &entity)
{
    if (!m_vehicles)
    {
        return nullptr;
    }

    VehicleSpawnData data;
    data.respawnDelay = Seconds(-1); // расставленные редактором машины не респавнятся
    data.modelID = entity.model;
    data.position = entity.position;
    data.zRotation = entity.rotation.z;
    data.colour1 = entity.colour1;
    data.colour2 = entity.colour2;
    data.siren = false;
    data.interior = 0;
    return m_vehicles->create(data);
}

void EditorSystem::createVehicleEntity(IPlayer &player, int model)
{
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

void EditorSystem::duplicateEntity(IPlayer &player, int index)
{
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
    }
    else
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

    state.entities.push_back(copy);
    state.selectedIndex = static_cast<int>(state.entities.size()) - 1;
    state.followCamera = false;
    applyEntityAnimation(state.entities.back());
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
        if (IVehicle *vehicle = m_vehicles ? m_vehicles->get(entity.entityId) : nullptr)
        {
            vehicle->setPosition(entity.position);
            vehicle->setZAngle(entity.rotation.z);
        }
    }
    else
    {
        if (IPickup *pickup = m_pickups ? m_pickups->get(entity.entityId) : nullptr)
        {
            pickup->setPosition(entity.position); // пикапы не вращаются — только позиция
        }
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
        if (m_vehicles)
        {
            m_vehicles->release(entity.entityId);
        }
    }
    else if (m_pickups)
    {
        m_pickups->release(entity.entityId);
    }

    state.entities.erase(state.entities.begin() + index);
    state.selectedIndex = -1;
    state.followCamera = false;
    state.keyMode = KeyMode::Camera;
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
            if (m_vehicles)
            {
                m_vehicles->release(entity.entityId);
            }
        }
        else if (m_pickups)
        {
            m_pickups->release(entity.entityId);
        }
    }
    state.entities.clear();
    state.selectedIndex = -1;
    state.followCamera = false;
    state.keyMode = KeyMode::Camera;
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
        if (IVehicle *vehicle = m_vehicles ? m_vehicles->get(entity.entityId) : nullptr)
        {
            vehicle->setPosition(parkPosition);
        }
    }
    else if (IPickup *pickup = m_pickups ? m_pickups->get(entity.entityId) : nullptr)
    {
        pickup->setPosition(parkPosition);
    }

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
        }
    }
    else
    {
        // FindZ не сработал (зона не прогружена на клиенте) — оставляем прежнюю высоту.
        player.sendClientMessage(Colour::White(),
                                 u("Не удалось найти землю: зона не прогружена. Подлетите ближе и повторите."));
    }

    applyEntityTransform(entity); // поднять сущность из укрытия на найденную землю (или вернуть как было)
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
        }
    }

    std::string body;
    body += "Создать объект\n";
    body += "Создать актора (NPC)\n";
    body += "Создать машину\n";
    body += "Создать пикап\n";
    body += fmt::format("Объекты ({})\n", objectCount);
    body += fmt::format("Акторы ({})\n", actorCount);
    body += fmt::format("Машины ({})\n", vehicleCount);
    body += fmt::format("Пикапы ({})\n", pickupCount);
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
                                 showObjectList(*player);
                                 break;
                             case 5:
                                 showActorList(*player);
                                 break;
                             case 6:
                                 showVehicleList(*player);
                                 break;
                             case 7:
                                 showPickupList(*player);
                                 break;
                             case 8:
                                 m_state[playerId].autoGround = !m_state[playerId].autoGround;
                                 showMain(*player);
                                 break;
                             case 9:
                                 showCameraSpeedInput(*player);
                                 break;
                             case 10:
                                 showSaveNameInput(*player);
                                 break;
                             case 11:
                                 showLoadList(*player);
                                 break;
                             case 12:
                                 showClearConfirm(*player);
                                 break;
                             case 13:
                                 break; // летать
                             case 14:
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

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
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
            case 2:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveXY;
                player->sendClientMessage(Colour::White(),
                                          u("Стрелки двигают объект (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 3:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveZ;
                player->sendClientMessage(
                    Colour::White(), u("Вверх/вниз меняют высоту (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 4:
                state.followCamera = false;
                state.keyMode = KeyMode::Rotate;
                player->sendClientMessage(
                    Colour::White(), u("Влево/вправо вращают объект (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 5:
                showKeyStepInput(*player);
                break;
            case 6: // снэп к земле
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под объектом..."));
                showObjectEdit(*player);
                break;
            case 7:
                showDistanceInput(*player);
                break;
            case 8:
                showPositionInput(*player);
                break;
            case 9:
                showRotationInput(*player);
                break;
            case 10:
                showChangeModelInput(*player);
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
                showObjectEdit(*player);
                break;
            case 13:
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

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
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
                state.followCamera = false;
                state.keyMode = KeyMode::MoveXY;
                player->sendClientMessage(Colour::White(),
                                          u("Стрелки двигают актора (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 3:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveZ;
                player->sendClientMessage(
                    Colour::White(), u("Вверх/вниз меняют высоту (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 4:
                state.followCamera = false;
                state.keyMode = KeyMode::Rotate;
                player->sendClientMessage(
                    Colour::White(), u("Влево/вправо вращают актора (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 5:
                showKeyStepInput(*player);
                break;
            case 6:
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под актором..."));
                showActorEdit(*player);
                break;
            case 7:
                showDistanceInput(*player);
                break;
            case 8:
                showPositionInput(*player);
                break;
            case 9:
                showAngleInput(*player);
                break;
            case 10:
                showChangeModelInput(*player);
                break;
            case 11:
                showAnimationMenu(*player);
                break;
            case 12: // переключить режим анимации
                entity.animLoop = !entity.animLoop;
                applyEntityAnimation(entity);
                showActorEdit(*player);
                break;
            case 13:
                entity.animLib.clear();
                entity.animName.clear();
                if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
                {
                    actor->clearAnimations();
                }
                showActorEdit(*player);
                break;
            case 14:
                duplicateEntity(*player, state.selectedIndex);
                showEntityEdit(*player);
                break;
            case 15: // перелететь
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
            case 16:
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

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
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
                state.followCamera = false;
                state.keyMode = KeyMode::MoveXY;
                player->sendClientMessage(Colour::White(),
                                          u("Стрелки двигают машину (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 3:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveZ;
                player->sendClientMessage(
                    Colour::White(), u("Вверх/вниз меняют высоту (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 4:
                state.followCamera = false;
                state.keyMode = KeyMode::Rotate;
                player->sendClientMessage(
                    Colour::White(), u("Влево/вправо вращают машину (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 5:
                showKeyStepInput(*player);
                break;
            case 6:
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под машиной..."));
                showVehicleEdit(*player);
                break;
            case 7:
                showDistanceInput(*player);
                break;
            case 8:
                showPositionInput(*player);
                break;
            case 9:
                showAngleInput(*player);
                break;
            case 10:
                showChangeModelInput(*player);
                break;
            case 11:
                showVehicleColoursInput(*player);
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
                showVehicleEdit(*player);
                break;
            case 14:
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

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
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
                state.followCamera = false;
                state.keyMode = KeyMode::MoveXY;
                player->sendClientMessage(Colour::White(),
                                          u("Стрелки двигают пикап (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 3:
                state.followCamera = false;
                state.keyMode = KeyMode::MoveZ;
                player->sendClientMessage(
                    Colour::White(), u("Вверх/вниз меняют высоту (Sprint — крупно, Alt — точно). /editor — готово."));
                break;
            case 4:
                showKeyStepInput(*player);
                break;
            case 5:
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под пикапом..."));
                showPickupEdit(*player);
                break;
            case 6:
                showDistanceInput(*player);
                break;
            case 7:
                showPositionInput(*player);
                break;
            case 8:
                showChangeModelInput(*player);
                break;
            case 9:
                showPickupTypeInput(*player);
                break;
            case 10:
                duplicateEntity(*player, state.selectedIndex);
                showEntityEdit(*player);
                break;
            case 11: // перелететь
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
            case 12:
                deleteEntity(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Пикап удалён"));
                showMain(*player);
                break;
            default:
                break;
            }
        });
}

void EditorSystem::showPickupModelInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Создать пикап", "Введите ID модели пикапа (напр. 1212 — деньги, 1240 — сердце)",
                   "Создать", "Назад"),
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

            int model = 0;
            if (!parseInt(text, model) || model < 0 || model > MAX_OBJECT_MODEL)
            {
                player->sendClientMessage(Colour::White(),
                                          u(fmt::format("Введите ID модели от 0 до {}", MAX_OBJECT_MODEL)));
                showPickupModelInput(*player);
                return;
            }

            createPickupEntity(*player, model);
            showEntityEdit(*player);
        });
}

void EditorSystem::showPickupTypeInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Тип пикапа",
                   u(fmt::format("Введите тип поведения (0-{}). Частые: 1 — статичный, 2 — исчезает и респавнится, "
                                 "14 — подбор из машины",
                                 MAX_PICKUP_TYPE)),
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
                int type = 0;
                if (!parseInt(text, type) || type < 0 || type > MAX_PICKUP_TYPE)
                {
                    player->sendClientMessage(Colour::White(),
                                              u(fmt::format("Введите тип от 0 до {}", MAX_PICKUP_TYPE)));
                    showPickupTypeInput(*player);
                    return;
                }

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
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Создать объект", "Введите ID модели объекта", "Создать", "Назад"),
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

            int model = 0;
            if (!parseInt(text, model) || model < 0 || model > MAX_OBJECT_MODEL)
            {
                player->sendClientMessage(Colour::White(),
                                          u(fmt::format("Введите ID модели от 0 до {}", MAX_OBJECT_MODEL)));
                showObjectModelInput(*player);
                return;
            }

            createObjectEntity(*player, model);
            showEntityEdit(*player);
        });
}

void EditorSystem::showActorSkinInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Создать актора", "Введите ID скина актора", "Создать", "Назад"),
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

            int skin = 0;
            if (!parseInt(text, skin) || skin < 0 || skin > MAX_ACTOR_SKIN)
            {
                player->sendClientMessage(Colour::White(),
                                          u(fmt::format("Введите ID скина от 0 до {}", MAX_ACTOR_SKIN)));
                showActorSkinInput(*player);
                return;
            }

            createActorEntity(*player, skin);
            showEntityEdit(*player);
        });
}

void EditorSystem::showVehicleModelInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Создать машину",
                   fmt::format("Введите ID модели машины ({}-{})", MIN_VEHICLE_MODEL, MAX_VEHICLE_MODEL), "Создать",
                   "Назад"),
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

            int model = 0;
            if (!parseInt(text, model) || model < MIN_VEHICLE_MODEL || model > MAX_VEHICLE_MODEL)
            {
                player->sendClientMessage(
                    Colour::White(), u(fmt::format("Введите ID модели от {} до {}", MIN_VEHICLE_MODEL, MAX_VEHICLE_MODEL)));
                showVehicleModelInput(*player);
                return;
            }

            createVehicleEntity(*player, model);
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
                if (IVehicle *vehicle = m_vehicles ? m_vehicles->get(entity.entityId) : nullptr)
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

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, isActor ? "Сменить скин" : "Сменить модель",
                   isActor ? "Введите новый ID скина" : "Введите новый ID модели", "OK", "Назад"),
        [this, playerId = player.getID(), type](DialogResponse response, int, StringView text)
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

                int model = 0;
                if (!parseInt(text, model) || model < minModel || model > maxModel)
                {
                    player->sendClientMessage(Colour::White(),
                                              u(fmt::format("Введите ID от {} до {}", minModel, maxModel)));
                    showChangeModelInput(*player);
                    return;
                }

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
                        if (m_vehicles)
                        {
                            m_vehicles->release(oldId);
                        }
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
                             if (name.empty() || name.find('/') != std::string::npos ||
                                 name.find('\\') != std::string::npos || name.find("..") != std::string::npos)
                             {
                                 player->sendClientMessage(Colour::White(), u("Недопустимое имя файла"));
                                 showSaveNameInput(*player);
                                 return;
                             }

                             std::string error;
                             if (saveToFile(m_state[playerId], name, error))
                             {
                                 player->sendClientMessage(
                                     Colour::White(), u(fmt::format("Карта сохранена: {}/{}.txt", MAPS_DIR, name)));
                             }
                             else
                             {
                                 player->sendClientMessage(Colour::White(), u("Ошибка сохранения: " + error));
                             }
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

void EditorSystem::showLoadList(IPlayer &player)
{
    std::vector<std::string> files = listMapFiles();

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
                showLoadList(*player);
                return;
            }

            if (listItem == 0)
            {
                clearScene(*player);
            }

            std::string error;
            std::size_t loaded = 0;
            if (loadFromFile(*player, fileName, error, loaded))
            {
                player->sendClientMessage(Colour::White(),
                                          u(fmt::format("Карта загружена: {} (сущностей: {})", fileName, loaded)));
            }
            else
            {
                player->sendClientMessage(Colour::White(), u("Ошибка загрузки: " + error));
            }
            showMain(*player);
        });
}

// ------------------------------------------------------------------ файлы

std::vector<std::string> EditorSystem::listMapFiles() const
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
}

bool EditorSystem::saveToFile(const EditorState &state, const std::string &name, std::string &error)
{
    std::error_code ec;
    std::filesystem::create_directories(MAPS_DIR, ec);

    const std::string path = MAPS_DIR + "/" + name + ".txt";
    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        error = "не удалось открыть файл " + path;
        return false;
    }

    for (const EditorEntity &e : state.entities)
    {
        if (e.type == EntityType::Object)
        {
            out << fmt::format("object {} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f}\n", e.model, e.position.x,
                               e.position.y, e.position.z, e.rotation.x, e.rotation.y, e.rotation.z);
        }
        else if (e.type == EntityType::Actor)
        {
            const std::string lib = e.animLib.empty() ? "-" : e.animLib;
            const std::string anim = e.animName.empty() ? "-" : e.animName;
            // Последний токен — режим анимации; старые файлы без него читаются как loop.
            out << fmt::format("actor {} {:.4f} {:.4f} {:.4f} {:.4f} {} {} {}\n", e.model, e.position.x, e.position.y,
                               e.position.z, e.rotation.z, lib, anim, e.animLoop ? "loop" : "freeze");
        }
        else if (e.type == EntityType::Vehicle)
        {
            out << fmt::format("vehicle {} {:.4f} {:.4f} {:.4f} {:.4f} {} {}\n", e.model, e.position.x, e.position.y,
                               e.position.z, e.rotation.z, e.colour1, e.colour2);
        }
        else
        {
            out << fmt::format("pickup {} {} {:.4f} {:.4f} {:.4f}\n", e.model, e.pickupType, e.position.x,
                               e.position.y, e.position.z);
        }
    }

    return true;
}

bool EditorSystem::loadFromFile(IPlayer &player, const std::string &name, std::string &error, std::size_t &loaded)
{
    const std::string path = MAPS_DIR + "/" + name + ".txt";
    std::ifstream in(path);
    if (!in)
    {
        error = "не удалось открыть файл " + path;
        return false;
    }

    EditorState &state = stateOf(player);
    loaded = 0;

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
    }

    if (loaded == 0)
    {
        error = "в файле нет валидных сущностей";
        return false;
    }
    return true;
}
