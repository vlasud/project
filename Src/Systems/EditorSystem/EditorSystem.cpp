#include "EditorSystem.h"

#include "../../Log/LogManager.h"
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
constexpr int EDITOR_DIALOG_ID = 32000;
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

EditorSystem::EditorSystem(ICore &core, const ServiceRegister &serviceRegister) : BaseSystem(core, serviceRegister)
{
    core.getPlayers().getPlayerTextDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);
}

void EditorSystem::initialize(IComponentList *components)
{
    m_objects = components->queryComponent<IObjectsComponent>();
    m_actors = components->queryComponent<IActorsComponent>();
    m_dialogs = components->queryComponent<IDialogsComponent>();

    if (m_dialogs)
    {
        m_dialogs->getEventDispatcher().addEventHandler(this);
    }
}

EditorSystem::EditorState &EditorSystem::stateOf(const IPlayer &player)
{
    return m_state[player.getID()];
}

// ------------------------------------------------------------------ commands

bool EditorSystem::onPlayerCommandText(IPlayer &player, StringView message)
{
    if (message != "/editor")
    {
        return false;
    }

    EditorState &state = stateOf(player);

    if (!state.enabled)
    {
        enableEditor(player);
        showMain(player);
        return true;
    }

    // Уже в редакторе: если редактируем сущность в режиме следования — возвращаемся к её редактированию.
    if (state.followCamera && state.selectedIndex >= 0 && state.selectedIndex < (int)state.entities.size())
    {
        if (state.entities[state.selectedIndex].type == EntityType::Object)
        {
            showObjectEdit(player);
        }
        else
        {
            showActorEdit(player);
        }
    }
    else
    {
        showMain(player);
    }
    return true;
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

    player.sendClientMessage(Colour::White(),
                             u("Редактор включён. WASD — полёт камеры, /editor — открыть меню."));
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
    state.screen = Screen::None;

    player.setCameraBehind();
    player.sendClientMessage(Colour::White(),
                             u("Редактор выключен. Расставленные объекты остались на сцене."));
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

// ------------------------------------------------------------------ dialogs

IPlayerDialogData *EditorSystem::getDialog(IPlayer &player)
{
    return queryExtension<IPlayerDialogData>(player);
}

void EditorSystem::showText(IPlayer &player, DialogStyle style, const std::string &title, const std::string &body,
                            const std::string &btnLeft, const std::string &btnRight)
{
    IPlayerDialogData *dialog = getDialog(player);
    if (!dialog)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось показать диалог"));
        return;
    }
    dialog->show(player, EDITOR_DIALOG_ID, style, u(title), u(body), u(btnLeft), u(btnRight));
}

void EditorSystem::showMain(IPlayer &player)
{
    EditorState &state = stateOf(player);
    state.screen = Screen::Main;

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

    showText(player, DialogStyle_LIST, "Редактор карты", body, "Выбрать", "Закрыть");
}

void EditorSystem::showObjectEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (state.selectedIndex < 0 || state.selectedIndex >= (int)state.entities.size())
    {
        showMain(player);
        return;
    }
    state.screen = Screen::ObjectEdit;
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

    showText(player, DialogStyle_LIST, fmt::format("Объект (модель {})", e.model), body, "Выбрать", "Назад");
}

void EditorSystem::showActorEdit(IPlayer &player)
{
    EditorState &state = stateOf(player);
    if (state.selectedIndex < 0 || state.selectedIndex >= (int)state.entities.size())
    {
        showMain(player);
        return;
    }
    state.screen = Screen::ActorEdit;
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

    showText(player, DialogStyle_LIST, fmt::format("Актор (скин {})", e.model), body, "Выбрать", "Назад");
}

void EditorSystem::showObjectList(IPlayer &player)
{
    EditorState &state = stateOf(player);
    state.screen = Screen::ObjectList;

    std::string body;
    for (size_t i = 0; i < state.entities.size(); ++i)
    {
        if (state.entities[i].type != EntityType::Object)
        {
            continue;
        }
        const EditorEntity &e = state.entities[i];
        body += fmt::format("#{}\tмодель {}\t{:.1f} {:.1f} {:.1f}\n", i, e.model, e.position.x, e.position.y,
                            e.position.z);
    }
    if (body.empty())
    {
        body = "Список пуст";
    }

    showText(player, DialogStyle_LIST, "Объекты на сцене", body, "Выбрать", "Назад");
}

void EditorSystem::showActorList(IPlayer &player)
{
    EditorState &state = stateOf(player);
    state.screen = Screen::ActorList;

    std::string body;
    for (size_t i = 0; i < state.entities.size(); ++i)
    {
        if (state.entities[i].type != EntityType::Actor)
        {
            continue;
        }
        const EditorEntity &e = state.entities[i];
        body += fmt::format("#{}\tскин {}\t{:.1f} {:.1f} {:.1f}\n", i, e.model, e.position.x, e.position.y,
                            e.position.z);
    }
    if (body.empty())
    {
        body = "Список пуст";
    }

    showText(player, DialogStyle_LIST, "Акторы на сцене", body, "Выбрать", "Назад");
}

void EditorSystem::showLoadList(IPlayer &player)
{
    EditorState &state = stateOf(player);
    state.screen = Screen::LoadList;
    state.loadFiles = listMapFiles();

    std::string body;
    for (const std::string &name : state.loadFiles)
    {
        body += name + "\n";
    }
    if (body.empty())
    {
        body = "Нет сохранённых карт";
    }

    showText(player, DialogStyle_LIST, "Загрузить карту", body, "Загрузить", "Назад");
}

// ------------------------------------------------------------------ response

void EditorSystem::onDialogResponse(IPlayer &player, int dialogId, DialogResponse response, int listItem,
                                    StringView inputText)
{
    if (dialogId != EDITOR_DIALOG_ID)
    {
        return;
    }

    EditorState &state = stateOf(player);
    if (!state.enabled)
    {
        return;
    }

    const bool ok = (response == DialogResponse_Left);
    const std::string text = inputText.to_string();

    const auto reshowEdit = [&]
    {
        if (state.selectedIndex >= 0 && state.selectedIndex < (int)state.entities.size() &&
            state.entities[state.selectedIndex].type == EntityType::Actor)
        {
            showActorEdit(player);
        }
        else
        {
            showObjectEdit(player);
        }
    };

    switch (state.screen)
    {
    case Screen::Main:
        handleMain(player, state, response, listItem);
        break;

    case Screen::ObjectModelInput:
    {
        if (!ok)
        {
            showMain(player);
            break;
        }
        int model = 0;
        if (!parseInt(inputText, model))
        {
            player.sendClientMessage(Colour::White(), u("Введите корректный числовой ID модели"));
            showText(player, DialogStyle_INPUT, "Создать объект", "Введите ID модели объекта", "Создать", "Назад");
            break;
        }
        createObjectEntity(player, model);
        showObjectEdit(player);
        break;
    }

    case Screen::ActorSkinInput:
    {
        if (!ok)
        {
            showMain(player);
            break;
        }
        int skin = 0;
        if (!parseInt(inputText, skin))
        {
            player.sendClientMessage(Colour::White(), u("Введите корректный числовой ID скина"));
            showText(player, DialogStyle_INPUT, "Создать актора", "Введите ID скина актора", "Создать", "Назад");
            break;
        }
        createActorEntity(player, skin);
        showActorEdit(player);
        break;
    }

    case Screen::ObjectEdit:
        handleObjectEdit(player, state, response, listItem);
        break;

    case Screen::ActorEdit:
        handleActorEdit(player, state, response, listItem);
        break;

    case Screen::ObjectList:
    {
        if (!ok)
        {
            showMain(player);
            break;
        }
        // Сопоставляем выбранную строку с индексом объекта.
        int seen = -1;
        for (size_t i = 0; i < state.entities.size(); ++i)
        {
            if (state.entities[i].type != EntityType::Object)
            {
                continue;
            }
            if (++seen == listItem)
            {
                state.selectedIndex = static_cast<int>(i);
                showObjectEdit(player);
                return;
            }
        }
        showMain(player);
        break;
    }

    case Screen::ActorList:
    {
        if (!ok)
        {
            showMain(player);
            break;
        }
        int seen = -1;
        for (size_t i = 0; i < state.entities.size(); ++i)
        {
            if (state.entities[i].type != EntityType::Actor)
            {
                continue;
            }
            if (++seen == listItem)
            {
                state.selectedIndex = static_cast<int>(i);
                showActorEdit(player);
                return;
            }
        }
        showMain(player);
        break;
    }

    case Screen::RotateInput:
    {
        if (!ok)
        {
            reshowEdit();
            break;
        }
        float angle = 0.0f;
        if (!parseFloat(text, angle) || state.selectedIndex < 0 ||
            state.selectedIndex >= (int)state.entities.size())
        {
            player.sendClientMessage(Colour::White(), u("Введите корректный угол"));
            reshowEdit();
            break;
        }
        state.entities[state.selectedIndex].rotation[state.rotateAxis] = angle;
        applyEntityTransform(state.entities[state.selectedIndex]);
        reshowEdit();
        break;
    }

    case Screen::PosInput:
    {
        if (!ok)
        {
            reshowEdit();
            break;
        }
        Vector3 pos;
        if (!parseVec3(text, pos) || state.selectedIndex < 0 || state.selectedIndex >= (int)state.entities.size())
        {
            player.sendClientMessage(Colour::White(), u("Введите три числа: X Y Z"));
            reshowEdit();
            break;
        }
        state.entities[state.selectedIndex].position = pos;
        applyEntityTransform(state.entities[state.selectedIndex]);
        reshowEdit();
        break;
    }

    case Screen::DistanceInput:
    {
        if (!ok)
        {
            reshowEdit();
            break;
        }
        float dist = 0.0f;
        if (!parseFloat(text, dist))
        {
            player.sendClientMessage(Colour::White(), u("Введите корректное число"));
            reshowEdit();
            break;
        }
        state.placeDistance = std::clamp(dist, 1.0f, 100.0f);
        reshowEdit();
        break;
    }

    case Screen::ActorAnimLibInput:
    {
        if (!ok)
        {
            showActorEdit(player);
            break;
        }
        state.pendingAnimLib = text;
        state.screen = Screen::ActorAnimNameInput;
        showText(player, DialogStyle_INPUT, "Анимация — название",
                 "Введите название анимации (например, dnce_M_b)", "Применить", "Назад");
        break;
    }

    case Screen::ActorAnimNameInput:
    {
        if (!ok)
        {
            showActorEdit(player);
            break;
        }
        if (state.selectedIndex >= 0 && state.selectedIndex < (int)state.entities.size())
        {
            EditorEntity &entity = state.entities[state.selectedIndex];
            entity.animLib = state.pendingAnimLib;
            entity.animName = text;
            if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
            {
                actor->applyAnimation(
                    AnimationData(4.1f, true, true, true, false, 0, entity.animLib, entity.animName));
            }
        }
        showActorEdit(player);
        break;
    }

    case Screen::SaveNameInput:
    {
        if (!ok)
        {
            showMain(player);
            break;
        }
        if (text.empty())
        {
            player.sendClientMessage(Colour::White(), u("Имя файла не может быть пустым"));
            showText(player, DialogStyle_INPUT, "Сохранить карту", "Введите имя файла", "Сохранить", "Назад");
            break;
        }
        std::string error;
        if (saveToFile(state, text, error))
        {
            player.sendClientMessage(Colour::White(),
                                     u(fmt::format("Карта сохранена: {}/{}.txt", MAPS_DIR, text)));
        }
        else
        {
            player.sendClientMessage(Colour::White(), u("Ошибка сохранения: " + error));
        }
        showMain(player);
        break;
    }

    case Screen::LoadList:
    {
        if (!ok || listItem < 0 || listItem >= (int)state.loadFiles.size())
        {
            showMain(player);
            break;
        }
        std::string error;
        if (loadFromFile(player, state.loadFiles[listItem], error))
        {
            player.sendClientMessage(Colour::White(), u("Карта загружена: " + state.loadFiles[listItem]));
        }
        else
        {
            player.sendClientMessage(Colour::White(), u("Ошибка загрузки: " + error));
        }
        showMain(player);
        break;
    }

    default:
        break;
    }
}

void EditorSystem::handleMain(IPlayer &player, EditorState &state, DialogResponse response, int listItem)
{
    if (response == DialogResponse_Right)
    {
        state.screen = Screen::None; // закрыть меню, продолжить полёт
        return;
    }

    switch (listItem)
    {
    case 0:
        state.screen = Screen::ObjectModelInput;
        showText(player, DialogStyle_INPUT, "Создать объект", "Введите ID модели объекта", "Создать", "Назад");
        break;
    case 1:
        state.screen = Screen::ActorSkinInput;
        showText(player, DialogStyle_INPUT, "Создать актора", "Введите ID скина актора", "Создать", "Назад");
        break;
    case 2:
        showObjectList(player);
        break;
    case 3:
        showActorList(player);
        break;
    case 4:
        state.autoGround = !state.autoGround;
        showMain(player);
        break;
    case 5:
        state.screen = Screen::SaveNameInput;
        showText(player, DialogStyle_INPUT, "Сохранить карту", "Введите имя файла", "Сохранить", "Назад");
        break;
    case 6:
        showLoadList(player);
        break;
    case 7:
        state.screen = Screen::None; // летать
        break;
    case 8:
        disableEditor(player);
        break;
    default:
        break;
    }
}

void EditorSystem::handleObjectEdit(IPlayer &player, EditorState &state, DialogResponse response, int listItem)
{
    if (response == DialogResponse_Right)
    {
        showMain(player);
        return;
    }
    if (state.selectedIndex < 0 || state.selectedIndex >= (int)state.entities.size())
    {
        showMain(player);
        return;
    }
    EditorEntity &entity = state.entities[state.selectedIndex];

    switch (listItem)
    {
    case 0: // поставить по взгляду
        entity.position = placementPoint(player);
        applyEntityTransform(entity);
        if (state.autoGround)
        {
            requestGroundSnap(player, state.selectedIndex);
        }
        showObjectEdit(player);
        break;
    case 1: // снэп к земле (FindZ)
        requestGroundSnap(player, state.selectedIndex);
        player.sendClientMessage(Colour::White(), u("Ищу землю под объектом..."));
        showObjectEdit(player);
        break;
    case 2: // следовать за взглядом
        state.followCamera = !state.followCamera;
        if (state.followCamera)
        {
            state.screen = Screen::None;
            player.sendClientMessage(Colour::White(),
                                     u("Объект следует за взглядом. Летайте, затем /editor чтобы зафиксировать."));
        }
        else
        {
            showObjectEdit(player);
        }
        break;
    case 3:
        state.screen = Screen::DistanceInput;
        showText(player, DialogStyle_INPUT, "Дистанция установки", "Введите дистанцию (1-100)", "OK", "Назад");
        break;
    case 4:
        state.rotateAxis = 0;
        state.screen = Screen::RotateInput;
        showText(player, DialogStyle_INPUT, "Поворот X", "Введите угол поворота по оси X", "OK", "Назад");
        break;
    case 5:
        state.rotateAxis = 1;
        state.screen = Screen::RotateInput;
        showText(player, DialogStyle_INPUT, "Поворот Y", "Введите угол поворота по оси Y", "OK", "Назад");
        break;
    case 6:
        state.rotateAxis = 2;
        state.screen = Screen::RotateInput;
        showText(player, DialogStyle_INPUT, "Поворот Z", "Введите угол поворота по оси Z", "OK", "Назад");
        break;
    case 7:
        state.screen = Screen::PosInput;
        showText(player, DialogStyle_INPUT, "Позиция", "Введите координаты: X Y Z", "OK", "Назад");
        break;
    case 8:
        deleteEntity(player, state.selectedIndex);
        player.sendClientMessage(Colour::White(), u("Объект удалён"));
        showMain(player);
        break;
    default:
        break;
    }
}

void EditorSystem::handleActorEdit(IPlayer &player, EditorState &state, DialogResponse response, int listItem)
{
    if (response == DialogResponse_Right)
    {
        showMain(player);
        return;
    }
    if (state.selectedIndex < 0 || state.selectedIndex >= (int)state.entities.size())
    {
        showMain(player);
        return;
    }
    EditorEntity &entity = state.entities[state.selectedIndex];

    switch (listItem)
    {
    case 0:
        entity.position = placementPoint(player);
        applyEntityTransform(entity);
        if (state.autoGround)
        {
            requestGroundSnap(player, state.selectedIndex);
        }
        showActorEdit(player);
        break;
    case 1: // снэп к земле (FindZ)
        requestGroundSnap(player, state.selectedIndex);
        player.sendClientMessage(Colour::White(), u("Ищу землю под актором..."));
        showActorEdit(player);
        break;
    case 2:
        state.followCamera = !state.followCamera;
        if (state.followCamera)
        {
            state.screen = Screen::None;
            player.sendClientMessage(Colour::White(),
                                     u("Актор следует за взглядом. Летайте, затем /editor чтобы зафиксировать."));
        }
        else
        {
            showActorEdit(player);
        }
        break;
    case 3:
        state.screen = Screen::DistanceInput;
        showText(player, DialogStyle_INPUT, "Дистанция установки", "Введите дистанцию (1-100)", "OK", "Назад");
        break;
    case 4:
        state.rotateAxis = 2;
        state.screen = Screen::RotateInput;
        showText(player, DialogStyle_INPUT, "Поворот актора", "Введите угол поворота (0-360)", "OK", "Назад");
        break;
    case 5:
        state.screen = Screen::ActorAnimLibInput;
        showText(player, DialogStyle_INPUT, "Анимация — библиотека",
                 "Введите библиотеку анимации (например, DANCING)", "Далее", "Назад");
        break;
    case 6:
        entity.animLib.clear();
        entity.animName.clear();
        if (IActor *actor = m_actors ? m_actors->get(entity.entityId) : nullptr)
        {
            actor->clearAnimations();
        }
        showActorEdit(player);
        break;
    case 7:
        deleteEntity(player, state.selectedIndex);
        player.sendClientMessage(Colour::White(), u("Актор удалён"));
        showMain(player);
        break;
    default:
        break;
    }
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
