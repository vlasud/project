#include "Systems/Core/DebugCameraSystem/DebugCameraSystem.h"

#include "Services/AdminService/AdminService.h"
#include "ThreadPool/ThreadPool.h"
#include "Utils/Encoding/Encoding.h"
#include "glm/geometric.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace
{
// Биты PlayerKeyData::keys (классические значения SA).
constexpr uint32_t KEY_CROUCH = 2;
constexpr uint32_t KEY_SPRINT = 8;
constexpr uint32_t KEY_JUMP = 32;

constexpr float SPRINT_MULTIPLIER = 5.0f;
constexpr float CAMERA_SPEED_MIN = 0.1f;
constexpr float CAMERA_SPEED_MAX = 10.0f;

constexpr float LOOKAT_DISTANCE = 10.0f; // насколько вперёд от камеры лежит точка взгляда

constexpr float SEGMENT_TIME_MIN_S = 0.2f;
constexpr float SEGMENT_TIME_MAX_S = 120.0f;
constexpr std::size_t MAX_POINTS = 64;

const std::string PATHS_DIR = "camerapaths";

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
} // namespace

DebugCameraSystem::DebugCameraSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_commandService(serviceRegister.getService<PlayerCommandService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_cameraService(serviceRegister.getService<CameraService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);

    m_commandService.add("camera", {},
                         [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                         {
                             CameraState &state = stateOf(player);

                             if (!state.enabled)
                             {
                                 enableCamera(player);
                                 if (stateOf(player).enabled)
                                 {
                                     showMain(player);
                                 }
                                 return;
                             }

                             if (m_cameraService.isPlaying(player.getID()))
                             {
                                 // Остановка: полёт продолжается из точки-цели текущего сегмента.
                                 const int n = static_cast<int>(state.points.size());
                                 const Vector3 rest =
                                     n >= 2 ? state.points[(m_cameraService.segmentIndex(player.getID()) + 1) % n].position
                                            : state.position;
                                 stopPlayback(player, rest);
                             }

                             showMain(player);
                         },
                         PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    m_commandService.add("cpoint", {},
                         [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                         {
                             CameraState &state = stateOf(player);
                             if (!state.enabled || m_cameraService.isPlaying(player.getID()))
                             {
                                 player.sendClientMessage(
                                     Colour::White(), u("Сначала включите камеру: /camera (и не во время проигрывания)"));
                                 return;
                             }
                             savePoint(player);
                         },
                         PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));

    m_commandService.add("cplay", {},
                         [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                         {
                             CameraState &state = stateOf(player);
                             if (!state.enabled)
                             {
                                 player.sendClientMessage(Colour::White(), u("Сначала включите камеру: /camera"));
                                 return;
                             }
                             if (m_cameraService.isPlaying(player.getID()))
                             {
                                 return; // уже играет
                             }
                             startPlayback(player);
                         },
                         PermissionSpec::admin(AdminService::DEVELOPER_LEVEL));
}

void DebugCameraSystem::initialize(IComponentList *components)
{
    m_objects = components->queryComponent<IObjectsComponent>();
}

DebugCameraSystem::CameraState &DebugCameraSystem::stateOf(const IPlayer &player)
{
    return m_state[player.getID()];
}

IPlayer *DebugCameraSystem::cameraPlayer(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player || !m_state[playerId].enabled)
    {
        return nullptr;
    }
    return player;
}

void DebugCameraSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    CameraState &state = stateOf(player);
    releaseCameraObject(state);
    state = CameraState{};
}

// ------------------------------------------------------------------ режим

void DebugCameraSystem::enableCamera(IPlayer &player)
{
    if (!m_objects)
    {
        player.sendClientMessage(Colour::White(), u("Компонент объектов недоступен"));
        return;
    }

    CameraState &state = stateOf(player);
    state.returnPosition = m_locationService.getPosition(player.getID());
    state.position = state.returnPosition + Vector3(0.0f, 0.0f, 3.0f);

    if (!createCameraObject(player))
    {
        player.sendClientMessage(Colour::White(), u("Не удалось создать объект камеры (лимит пула)"));
        return;
    }

    state.enabled = true;
    player.sendClientMessage(Colour::White(), u("Камера включена. WASD — полёт, Jump/C — вверх/вниз, Sprint — быстрее."));
    player.sendClientMessage(Colour::White(), u("/cpoint — сохранить точку, /cplay — проиграть путь, /camera — меню."));
}

void DebugCameraSystem::disableCamera(IPlayer &player)
{
    CameraState &state = stateOf(player);

    m_cameraService.stop(player, false); // setCameraBehind ниже сделаем сами
    releaseCameraObject(state);
    state.enabled = false;
    state.selectedPoint = -1;
    // Точки не трогаем: можно вернуться в камеру и проиграть путь снова.

    m_locationService.teleport(player, state.returnPosition);
    player.setCameraBehind();
    player.sendClientMessage(Colour::White(), u("Камера выключена. Точки пути сохранены до выхода с сервера."));
}

bool DebugCameraSystem::createCameraObject(IPlayer &player)
{
    CameraState &state = stateOf(player);
    IObject *object = m_objects ? m_objects->create(0, state.position, Vector3(0.0f, 0.0f, 0.0f)) : nullptr;
    if (!object)
    {
        return false;
    }
    state.objectId = object->getID();
    player.attachCameraToObject(*object);
    return true;
}

void DebugCameraSystem::releaseCameraObject(CameraState &state)
{
    if (state.objectId >= 0 && m_objects)
    {
        m_objects->release(state.objectId);
    }
    state.objectId = -1;
}

// ------------------------------------------------------------------ per-tick

bool DebugCameraSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    CameraState &state = stateOf(player);
    if (!state.enabled)
    {
        return true;
    }

    // Проигрыванием рулит CameraService (таймеры) — клавиши полёта не мешают.
    if (m_cameraService.isPlaying(player.getID()))
    {
        return true;
    }

    processFlight(player);
    return true;
}

void DebugCameraSystem::processFlight(IPlayer &player)
{
    CameraState &state = stateOf(player);

    const PlayerAimData &aim = player.getAimData();
    Vector3 forward = aim.camFrontVector;
    const float len = glm::length(forward);
    if (len < 0.0001f)
    {
        return;
    }
    forward /= len;

    const PlayerKeyData &keyData = player.getKeyData();
    const float speed = state.speed * ((keyData.keys & KEY_SPRINT) ? SPRINT_MULTIPLIER : 1.0f);
    bool moved = false;

    if (keyData.upDown)
    {
        state.position += forward * (keyData.upDown > 0 ? -speed : speed);
        moved = true;
    }

    if (keyData.leftRight)
    {
        Vector3 right = glm::vec3(forward.y, -forward.x, 0.0f);
        const float rlen = glm::length(right);
        if (rlen > 0.0001f)
        {
            right /= rlen;
            state.position += right * (keyData.leftRight > 0 ? speed : -speed);
            moved = true;
        }
    }

    if (keyData.keys & KEY_JUMP)
    {
        state.position.z += speed;
        moved = true;
    }
    if (keyData.keys & KEY_CROUCH)
    {
        state.position.z -= speed;
        moved = true;
    }

    if (moved && state.objectId >= 0 && m_objects)
    {
        if (IObject *object = m_objects->get(state.objectId))
        {
            object->setPosition(state.position);
        }
    }
}

// ------------------------------------------------------------------ точки и проигрывание

Vector3 DebugCameraSystem::lookAtPoint(IPlayer &player) const
{
    const CameraState &state = m_state[player.getID()];
    Vector3 forward = player.getAimData().camFrontVector;
    const float len = glm::length(forward);
    if (len < 0.0001f)
    {
        return state.position + Vector3(LOOKAT_DISTANCE, 0.0f, 0.0f);
    }
    return state.position + forward / len * LOOKAT_DISTANCE;
}

void DebugCameraSystem::savePoint(IPlayer &player)
{
    CameraState &state = stateOf(player);
    if (state.points.size() >= MAX_POINTS)
    {
        player.sendClientMessage(Colour::White(), u(fmt::format("Достигнут лимит точек ({})", MAX_POINTS)));
        return;
    }

    state.points.push_back(CameraPoint{state.position, lookAtPoint(player)});
    player.sendClientMessage(Colour::White(),
                             u(fmt::format("Точка #{} сохранена. /cplay — проиграть.", state.points.size() - 1)));
}

void DebugCameraSystem::startPlayback(IPlayer &player)
{
    CameraState &state = stateOf(player);
    if (state.points.size() < 2)
    {
        player.sendClientMessage(Colour::White(), u("Нужно минимум две точки (/cpoint в разных местах)"));
        return;
    }

    // Камера должна быть свободна: привязка к объекту перебивает интерполяцию.
    releaseCameraObject(state);

    CameraPath path;
    path.points = state.points;
    path.segmentTime = Milliseconds(state.segmentTimeMs);
    path.loop = state.loop;

    m_cameraService.play(player, path,
                         [this, playerId = player.getID()](IPlayer &p)
                         {
                             // Незацикленный путь дошёл до конца — полёт из последней точки.
                             CameraState &state = m_state[playerId];
                             if (!state.enabled)
                             {
                                 return;
                             }
                             stopPlayback(p, state.points.empty() ? state.position : state.points.back().position);
                             p.sendClientMessage(Colour::White(), u("Перелёт завершён. Камера в последней точке."));
                         });

    player.sendClientMessage(Colour::White(), u("Проигрывание пути. /camera — остановить."));
}

void DebugCameraSystem::stopPlayback(IPlayer &player, const Vector3 &restPosition)
{
    CameraState &state = stateOf(player);
    m_cameraService.stop(player, false); // полётную камеру привяжем сами
    state.position = restPosition;

    if (!createCameraObject(player))
    {
        player.sendClientMessage(Colour::White(), u("Не удалось вернуть полётную камеру (лимит пула объектов)"));
    }
}

// ------------------------------------------------------------------ экраны диалогов

void DebugCameraSystem::showMain(IPlayer &player)
{
    CameraState &state = stateOf(player);

    std::string body;
    body += fmt::format("Сохранить точку здесь (всего: {})\n", state.points.size());
    body += fmt::format("Точки пути ({})\n", state.points.size());
    body += fmt::format("Проиграть путь ({} точек)\n", state.points.size());
    body += fmt::format("Время сегмента: {:.1f} сек\n", state.segmentTimeMs / 1000.0f);
    body += fmt::format("Зацикливание: {}\n", state.loop ? "ВКЛ" : "выкл");
    body += fmt::format("Скорость полёта: {:.1f}\n", state.speed);
    body += "Сохранить путь в файл\n";
    body += "Загрузить путь из файла\n";
    body += "Очистить точки\n";
    body += "Закрыть меню (летать)\n";
    body += "Выйти из камеры";

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Камера", body, "Выбрать", "Закрыть"),
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = cameraPlayer(playerId);
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
                                 savePoint(*player);
                                 showMain(*player);
                                 break;
                             case 1:
                                 showPoints(*player);
                                 break;
                             case 2:
                                 startPlayback(*player);
                                 break;
                             case 3:
                                 showSegmentTimeInput(*player);
                                 break;
                             case 4:
                                 m_state[playerId].loop = !m_state[playerId].loop;
                                 showMain(*player);
                                 break;
                             case 5:
                                 showSpeedInput(*player);
                                 break;
                             case 6:
                                 showSaveNameInput(*player);
                                 break;
                             case 7:
                                 listPathFilesAsync(*player); // листинг каталога на воркере
                                 break;
                             case 8:
                                 m_state[playerId].points.clear();
                                 m_state[playerId].selectedPoint = -1;
                                 player->sendClientMessage(Colour::White(), u("Точки удалены"));
                                 showMain(*player);
                                 break;
                             case 9:
                                 break; // летать
                             case 10:
                                 disableCamera(*player);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void DebugCameraSystem::showPoints(IPlayer &player)
{
    CameraState &state = stateOf(player);

    std::string body;
    for (std::size_t i = 0; i < state.points.size(); ++i)
    {
        const CameraPoint &p = state.points[i];
        body += fmt::format("#{}\t{:.1f} {:.1f} {:.1f}\n", i, p.position.x, p.position.y, p.position.z);
    }
    if (body.empty())
    {
        body = "Точек нет — сохраните /cpoint в полёте";
    }

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Точки пути", body, "Выбрать", "Назад"),
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = cameraPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             CameraState &state = m_state[playerId];
                             if (response != DialogResponse_Left || listItem < 0 ||
                                 listItem >= (int)state.points.size())
                             {
                                 showMain(*player);
                                 return;
                             }

                             state.selectedPoint = listItem;
                             showPointEdit(*player);
                         });
}

void DebugCameraSystem::showPointEdit(IPlayer &player)
{
    CameraState &state = stateOf(player);
    if (state.selectedPoint < 0 || state.selectedPoint >= (int)state.points.size())
    {
        showPoints(player);
        return;
    }

    std::string body;
    body += "Перелететь к точке\n";
    body += "Заменить текущей позицией камеры\n";
    body += "Удалить";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Точка #{}", state.selectedPoint), body, "Выбрать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = cameraPlayer(playerId);
            if (!player)
            {
                return;
            }

            CameraState &state = m_state[playerId];
            if (response != DialogResponse_Left || state.selectedPoint < 0 ||
                state.selectedPoint >= (int)state.points.size())
            {
                showPoints(*player);
                return;
            }

            switch (listItem)
            {
            case 0: // перелететь
                state.position = state.points[state.selectedPoint].position;
                if (state.objectId >= 0 && m_objects)
                {
                    if (IObject *object = m_objects->get(state.objectId))
                    {
                        object->setPosition(state.position);
                    }
                }
                showPointEdit(*player);
                break;
            case 1: // заменить
                state.points[state.selectedPoint] = CameraPoint{state.position, lookAtPoint(*player)};
                player->sendClientMessage(Colour::White(),
                                          u(fmt::format("Точка #{} обновлена", state.selectedPoint)));
                showPointEdit(*player);
                break;
            case 2: // удалить
                state.points.erase(state.points.begin() + state.selectedPoint);
                state.selectedPoint = -1;
                showPoints(*player);
                break;
            default:
                break;
            }
        });
}

void DebugCameraSystem::showSaveNameInput(IPlayer &player)
{
    CameraState &state = stateOf(player);
    if (state.points.empty())
    {
        player.sendClientMessage(Colour::White(), u("Нечего сохранять: точек нет"));
        showMain(player);
        return;
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Сохранить путь", "Введите имя файла", "Сохранить", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = cameraPlayer(playerId);
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
            if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
                name.find("..") != std::string::npos)
            {
                player->sendClientMessage(Colour::White(), u("Недопустимое имя файла"));
                showSaveNameInput(*player);
                return;
            }

            savePathToFileAsync(*player, name); // запись на воркере, сообщение придёт из колбэка
            showMain(*player);
        });
}

void DebugCameraSystem::showLoadList(IPlayer &player, std::vector<std::string> files)
{
    std::string body;
    for (const std::string &name : files)
    {
        body += name + "\n";
    }
    if (body.empty())
    {
        body = "Нет сохранённых путей";
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Загрузить путь", body, "Загрузить", "Назад"),
        [this, playerId = player.getID(), files = std::move(files)](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = cameraPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response != DialogResponse_Left || listItem < 0 || listItem >= (int)files.size())
            {
                showMain(*player);
                return;
            }

            loadPathFromFileAsync(*player, files[listItem]); // чтение на воркере, итог из колбэка
        });
}

void DebugCameraSystem::showSegmentTimeInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Время сегмента",
                   fmt::format("Введите время перелёта между точками в секундах ({}-{})", SEGMENT_TIME_MIN_S,
                               SEGMENT_TIME_MAX_S),
                   "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = cameraPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                float seconds = 0.0f;
                if (parseFloat(text.to_string(), seconds))
                {
                    seconds = std::clamp(seconds, SEGMENT_TIME_MIN_S, SEGMENT_TIME_MAX_S);
                    m_state[playerId].segmentTimeMs = static_cast<int>(seconds * 1000.0f);
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите корректное число секунд"));
                }
            }

            showMain(*player);
        });
}

// ------------------------------------------------------------------ файлы
// Диск — на воркерах тредпула; сериализация/парсинг и состояние камеры — на
// главном потоке.

std::string DebugCameraSystem::serializePath(const CameraState &state) const
{
    std::string out = fmt::format("settings {} {}\n", state.segmentTimeMs, state.loop ? 1 : 0);
    for (const CameraPoint &p : state.points)
    {
        out += fmt::format("point {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f}\n", p.position.x, p.position.y,
                           p.position.z, p.lookAt.x, p.lookAt.y, p.lookAt.z);
    }
    return out;
}

void DebugCameraSystem::savePathToFileAsync(IPlayer &player, const std::string &name)
{
    const std::string path = PATHS_DIR + "/" + name + ".txt";

    ThreadPool::Task<bool> task;
    task.func = [path, content = serializePath(stateOf(player))]()
    {
        std::error_code ec;
        std::filesystem::create_directories(PATHS_DIR, ec);
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
        if (IPlayer *player = cameraPlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Путь сохранён: " + path));
        }
    };
    task.errorCallback = [this, playerId = player.getID()](const std::string &error)
    {
        if (IPlayer *player = cameraPlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Ошибка сохранения: " + error));
        }
    };
    ThreadPool::addTask(std::move(task));
}

void DebugCameraSystem::loadPathFromFileAsync(IPlayer &player, const std::string &name)
{
    const std::string path = PATHS_DIR + "/" + name + ".txt";

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
    task.callback = [this, playerId = player.getID(), name](std::string content)
    {
        IPlayer *player = cameraPlayer(playerId);
        if (!player)
        {
            return; // камера выключена, пока читали — путь не трогаем
        }

        if (loadPathFromContent(stateOf(*player), content))
        {
            player->sendClientMessage(Colour::White(),
                                      u(fmt::format("Путь загружен: {} (точек: {}). /cplay — проиграть.", name,
                                                    stateOf(*player).points.size())));
        }
        else
        {
            player->sendClientMessage(Colour::White(), u("Ошибка загрузки: в файле нет валидных точек"));
        }
        showMain(*player);
    };
    task.errorCallback = [this, playerId = player.getID()](const std::string &error)
    {
        if (IPlayer *player = cameraPlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Ошибка загрузки: " + error));
            showMain(*player);
        }
    };
    ThreadPool::addTask(std::move(task));
}

void DebugCameraSystem::listPathFilesAsync(IPlayer &player)
{
    ThreadPool::Task<std::vector<std::string>> task;
    task.func = []()
    {
        std::vector<std::string> result;
        std::error_code ec;
        if (!std::filesystem::exists(PATHS_DIR, ec))
        {
            return result;
        }
        for (const auto &entry : std::filesystem::directory_iterator(PATHS_DIR, ec))
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
        if (IPlayer *player = cameraPlayer(playerId))
        {
            showLoadList(*player, std::move(files));
        }
    };
    ThreadPool::addTask(std::move(task));
}

bool DebugCameraSystem::loadPathFromContent(CameraState &state, const std::string &content)
{
    // Формат и клампы общие с CameraService — один парсер на тулзу и бизнес.
    CameraPath path;
    if (!CameraService::parsePath(content, path))
    {
        return false;
    }

    // Загрузка заменяет текущий путь целиком (вместе с настройками из файла).
    state.points = std::move(path.points);
    state.selectedPoint = -1;
    state.segmentTimeMs = static_cast<int>(path.segmentTime.count());
    state.loop = path.loop;
    return true;
}

void DebugCameraSystem::showSpeedInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Скорость полёта",
                   fmt::format("Введите скорость ({}-{})", CAMERA_SPEED_MIN, CAMERA_SPEED_MAX), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = cameraPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                float speed = 0.0f;
                if (parseFloat(text.to_string(), speed))
                {
                    m_state[playerId].speed = std::clamp(speed, CAMERA_SPEED_MIN, CAMERA_SPEED_MAX);
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите корректное число"));
                }
            }

            showMain(*player);
        });
}
