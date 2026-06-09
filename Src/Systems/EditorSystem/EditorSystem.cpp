#include "EditorSystem.h"

#include "../../Utils/Encoding/Encoding.h"
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
constexpr float FREE_CAM_SPEED = 1.0f;
const std::string MAPS_DIR = "maps";

// Высота, с которой клиент ищет землю (выше самой высокой точки карты ~ г. Чилиад).
constexpr float GROUND_PROBE_Z = 1500.0f;
constexpr int GROUND_PROBE_WAIT = 10;    // тиков ожидания ответа клиента перед чтением Z
constexpr float PROBE_PARK_Z = -1000.0f; // куда временно прячем сущность, чтобы FindZ не попал в неё

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
} // namespace

EditorSystem::EditorSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_commandService(serviceRegister.getService<PlayerCommandService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);

    m_commandService.add("editor", {},
                         [this](IPlayer &player, ...)
                         {
                             EditorState &state = stateOf(player);

                             if (!state.enabled)
                             {
                                 enableEditor(player);
                                 showMain(player);
                                 return true;
                             }

                             // В режиме следования /editor фиксирует сущность и возвращает в её редактирование.
                             if (state.followCamera && state.selectedIndex >= 0 &&
                                 state.selectedIndex < (int)state.entities.size())
                             {
                                 state.followCamera = false;
                                 showEntityEdit(player);
                             }
                             else
                             {
                                 showMain(player);
                             }
                         });
}

void EditorSystem::initialize(IComponentList *components)
{
    m_objects = components->queryComponent<IObjectsComponent>();
    m_actors = components->queryComponent<IActorsComponent>();
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

void EditorSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    EditorState &state = stateOf(player);
    if (state.cameraObjectId >= 0 && m_objects)
    {
        m_objects->release(state.cameraObjectId);
    }
    // Расставленные объекты/акторы остаются в мире, но сессия редактора слота сбрасывается.
    state = EditorState{};
}

void EditorSystem::enableEditor(IPlayer &player)
{
    if (!m_objects)
    {
        player.sendClientMessage(Colour::White(), u("Компонент объектов недоступен"));
        return;
    }

    EditorState &state = stateOf(player);
    state.enabled = true;
    state.cameraPosition = player.getPosition() + Vector3(0.0f, 0.0f, 5.0f);

    IObject *camObject = m_objects->create(0, state.cameraPosition, Vector3(0.0f, 0.0f, 0.0f));
    if (camObject)
    {
        state.cameraObjectId = camObject->getID();
        player.attachCameraToObject(*camObject);
    }

    player.sendClientMessage(Colour::White(), u("Редактор включён. WASD — полёт камеры, /editor — открыть меню."));
}

void EditorSystem::disableEditor(IPlayer &player)
{
    EditorState &state = stateOf(player);

    if (state.cameraObjectId >= 0 && m_objects)
    {
        m_objects->release(state.cameraObjectId);
    }
    state.cameraObjectId = -1;
    state.enabled = false;
    state.followCamera = false;
    state.groundProbe = -1;

    player.setCameraBehind();
    player.sendClientMessage(Colour::White(), u("Редактор выключен. Расставленные объекты остались на сцене."));
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

    const PlayerAimData &aim = player.getAimData();
    Vector3 forward = aim.camFrontVector;
    const float len = glm::length(forward);
    if (len < 0.0001f)
    {
        return true;
    }
    forward /= len;

    const PlayerKeyData &keyData = player.getKeyData();

    if (keyData.upDown)
    {
        state.cameraPosition += forward * (keyData.upDown > 0 ? -FREE_CAM_SPEED : FREE_CAM_SPEED);
    }

    if (keyData.leftRight)
    {
        Vector3 right = glm::vec3(forward.y, -forward.x, 0.0f);
        const float rlen = glm::length(right);
        if (rlen > 0.0001f)
        {
            right /= rlen;
            state.cameraPosition += right * (keyData.leftRight > 0 ? FREE_CAM_SPEED : -FREE_CAM_SPEED);
        }
    }

    if (state.cameraObjectId >= 0 && m_objects)
    {
        if (IObject *camObject = m_objects->get(state.cameraObjectId))
        {
            camObject->setPosition(state.cameraPosition);
        }
    }

    // Привязанная к взгляду сущность следует за камерой.
    if (state.followCamera && state.selectedIndex >= 0 && state.selectedIndex < (int)state.entities.size())
    {
        EditorEntity &entity = state.entities[state.selectedIndex];
        entity.position = placementPoint(player);
        applyEntityTransform(entity);
    }

    return true;
}

Vector3 EditorSystem::placementPoint(IPlayer &player) const
{
    const PlayerAimData &aim = player.getAimData();
    Vector3 forward = aim.camFrontVector;
    const float len = glm::length(forward);
    if (len < 0.0001f)
    {
        return player.getPosition();
    }
    forward /= len;

    const EditorState &state = m_state[player.getID()];
    return aim.camPos + forward * state.placeDistance;
}

// ------------------------------------------------------------------ entities

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
    entity.rotation = Vector3(0.0f, 0.0f, 0.0f);

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
    entity.rotation = Vector3(0.0f, 0.0f, 0.0f);

    state.entities.push_back(entity);
    state.selectedIndex = static_cast<int>(state.entities.size()) - 1;
    state.followCamera = false;

    if (state.autoGround)
    {
        requestGroundSnap(player, state.selectedIndex);
    }
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
    else
    {
        if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
        {
            actor->setPosition(entity.position);
            actor->setRotation(GTAQuat(entity.rotation));
        }
    }
}

void EditorSystem::deleteEntity(IPlayer &player, int index)
{
    EditorState &state = stateOf(player);
    if (index < 0 || index >= (int)state.entities.size())
    {
        return;
    }

    EditorEntity &entity = state.entities[index];
    if (entity.type == EntityType::Object)
    {
        if (m_objects)
        {
            m_objects->release(entity.entityId);
        }
    }
    else
    {
        if (m_actors)
        {
            m_actors->release(entity.entityId);
        }
    }

    state.entities.erase(state.entities.begin() + index);
    state.selectedIndex = -1;
    state.followCamera = false;
    state.groundProbe = -1; // индексы сдвинулись — отменяем активный зонд
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
    if (entity.type == EntityType::Object)
    {
        if (IObject *object = m_objects ? m_objects->get(entity.entityId) : nullptr)
        {
            object->setPosition(Vector3(target.x, target.y, PROBE_PARK_Z));
        }
    }
    else
    {
        if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
        {
            actor->setPosition(Vector3(target.x, target.y, PROBE_PARK_Z));
        }
    }

    state.groundProbe = index;
    state.groundProbeTicks = 0;
    player.setPositionFindZ(Vector3(target.x, target.y, GROUND_PROBE_Z));
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
    const float z = player.getPosition().z;

    if (z < GROUND_PROBE_Z - 1.0f)
    {
        entity.position.z = z; // нашли землю
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

// ------------------------------------------------------------------ dialog screens

void EditorSystem::showMain(IPlayer &player)
{
    EditorState &state = stateOf(player);

    size_t objectCount = 0;
    size_t actorCount = 0;
    for (const EditorEntity &e : state.entities)
    {
        (e.type == EntityType::Object ? objectCount : actorCount)++;
    }

    std::string body;
    body += "Создать объект\n";
    body += "Создать актора\n";
    body += fmt::format("Объекты ({})\n", objectCount);
    body += fmt::format("Акторы ({})\n", actorCount);
    body += fmt::format("Автоснап к земле: {}\n", state.autoGround ? "ВКЛ" : "выкл");
    body += "Сохранить в файл\n";
    body += "Загрузить из файла\n";
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
                                 showObjectList(*player);
                                 break;
                             case 3:
                                 showActorList(*player);
                                 break;
                             case 4:
                                 m_state[playerId].autoGround = !m_state[playerId].autoGround;
                                 showMain(*player);
                                 break;
                             case 5:
                                 showSaveNameInput(*player);
                                 break;
                             case 6:
                                 showLoadList(*player);
                                 break;
                             case 7:
                                 break; // летать
                             case 8:
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
    if (state.selectedIndex < 0 || state.selectedIndex >= (int)state.entities.size())
    {
        showMain(player);
        return;
    }

    if (state.entities[state.selectedIndex].type == EntityType::Actor)
    {
        showActorEdit(player);
    }
    else
    {
        showObjectEdit(player);
    }
}

void EditorSystem::showObjectEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (state.selectedIndex < 0 || state.selectedIndex >= (int)state.entities.size())
    {
        showMain(player);
        return;
    }
    const EditorEntity &e = state.entities[state.selectedIndex];

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += "Снэп к земле (FindZ)\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
    body += fmt::format("Дистанция установки: {:.1f}\n", state.placeDistance);
    body += fmt::format("Поворот X: {:.1f}\n", e.rotation.x);
    body += fmt::format("Поворот Y: {:.1f}\n", e.rotation.y);
    body += fmt::format("Поворот Z: {:.1f}\n", e.rotation.z);
    body += "Задать позицию (X Y Z)\n";
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
            if (response == DialogResponse_Right || state.selectedIndex < 0 ||
                state.selectedIndex >= (int)state.entities.size())
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
            case 1: // снэп к земле
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под объектом..."));
                showObjectEdit(*player);
                break;
            case 2: // следовать за взглядом
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
            case 3:
                showDistanceInput(*player);
                break;
            case 4:
                showRotateInput(*player, 0);
                break;
            case 5:
                showRotateInput(*player, 1);
                break;
            case 6:
                showRotateInput(*player, 2);
                break;
            case 7:
                showPosInput(*player);
                break;
            case 8:
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
    if (state.selectedIndex < 0 || state.selectedIndex >= (int)state.entities.size())
    {
        showMain(player);
        return;
    }
    const EditorEntity &e = state.entities[state.selectedIndex];

    std::string body;
    body += "Поставить по взгляду камеры\n";
    body += "Снэп к земле (FindZ)\n";
    body += fmt::format("Следовать за взглядом: {}\n", state.followCamera ? "ВКЛ" : "выкл");
    body += fmt::format("Дистанция установки: {:.1f}\n", state.placeDistance);
    body += fmt::format("Поворот (угол): {:.1f}\n", e.rotation.z);
    body += "Задать анимацию\n";
    body += "Очистить анимацию\n";
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
            if (response == DialogResponse_Right || state.selectedIndex < 0 ||
                state.selectedIndex >= (int)state.entities.size())
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
                requestGroundSnap(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Ищу землю под актором..."));
                showActorEdit(*player);
                break;
            case 2:
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
            case 3:
                showDistanceInput(*player);
                break;
            case 4:
                showRotateInput(*player, 2);
                break;
            case 5:
                showAnimLibInput(*player);
                break;
            case 6:
                entity.animLib.clear();
                entity.animName.clear();
                if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
                {
                    actor->clearAnimations();
                }
                showActorEdit(*player);
                break;
            case 7:
                deleteEntity(*player, state.selectedIndex);
                player->sendClientMessage(Colour::White(), u("Актор удалён"));
                showMain(*player);
                break;
            default:
                break;
            }
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
            if (!parseInt(text, model))
            {
                player->sendClientMessage(Colour::White(), u("Введите корректный числовой ID модели"));
                showObjectModelInput(*player);
                return;
            }

            createObjectEntity(*player, model);
            showEntityEdit(*player);
        });
}

void EditorSystem::showActorSkinInput(IPlayer &player)
{
    m_dialogService.show(player,
                         makeDialog(DialogStyle_INPUT, "Создать актора", "Введите ID скина актора", "Создать", "Назад"),
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
                             if (!parseInt(text, skin))
                             {
                                 player->sendClientMessage(Colour::White(), u("Введите корректный числовой ID скина"));
                                 showActorSkinInput(*player);
                                 return;
                             }

                             createActorEntity(*player, skin);
                             showEntityEdit(*player);
                         });
}

void EditorSystem::showRotateInput(IPlayer &player, int axis)
{
    static const char *axisNames[3] = {"X", "Y", "Z"};
    const std::string title = fmt::format("Поворот {}", axisNames[axis]);

    m_dialogService.show(player,
                         makeDialog(DialogStyle_INPUT, title, "Введите угол поворота в градусах", "OK", "Назад"),
                         [this, playerId = player.getID(), axis](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             EditorState &state = m_state[playerId];

                             if (response == DialogResponse_Left && state.selectedIndex >= 0 &&
                                 state.selectedIndex < (int)state.entities.size())
                             {
                                 float angle = 0.0f;
                                 if (parseFloat(text.to_string(), angle))
                                 {
                                     state.entities[state.selectedIndex].rotation[axis] = angle;
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

void EditorSystem::showPosInput(IPlayer &player)
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

                             if (response == DialogResponse_Left && state.selectedIndex >= 0 &&
                                 state.selectedIndex < (int)state.entities.size())
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
                                 showEntityEdit(*player);
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

            if (response == DialogResponse_Left && state.selectedIndex >= 0 &&
                state.selectedIndex < (int)state.entities.size())
            {
                EditorEntity &entity = state.entities[state.selectedIndex];
                entity.animLib = animLib;
                entity.animName = text.to_string();
                if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
                {
                    actor->applyAnimation(
                        AnimationData(4.1f, true, true, true, false, 0, entity.animLib, entity.animName));
                }
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
        player, makeDialog(DialogStyle_LIST, "Загрузить карту", body, "Загрузить", "Назад"),
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

            std::string error;
            if (loadFromFile(*player, files[listItem], error))
            {
                player->sendClientMessage(Colour::White(), u("Карта загружена: " + files[listItem]));
            }
            else
            {
                player->sendClientMessage(Colour::White(), u("Ошибка загрузки: " + error));
            }
            showMain(*player);
        });
}

// ------------------------------------------------------------------ files

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
        else
        {
            const std::string lib = e.animLib.empty() ? "-" : e.animLib;
            const std::string anim = e.animName.empty() ? "-" : e.animName;
            out << fmt::format("actor {} {:.4f} {:.4f} {:.4f} {:.4f} {} {}\n", e.model, e.position.x, e.position.y,
                               e.position.z, e.rotation.z, lib, anim);
        }
    }

    return true;
}

bool EditorSystem::loadFromFile(IPlayer &player, const std::string &name, std::string &error)
{
    const std::string path = MAPS_DIR + "/" + name + ".txt";
    std::ifstream in(path);
    if (!in)
    {
        error = "не удалось открыть файл " + path;
        return false;
    }

    EditorState &state = stateOf(player);

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
            if (lib != "-" && anim != "-")
            {
                entity.animLib = lib;
                entity.animName = anim;
                actor->applyAnimation(AnimationData(4.1f, true, true, true, false, 0, lib, anim));
            }
            state.entities.push_back(entity);
        }
    }

    return true;
}
