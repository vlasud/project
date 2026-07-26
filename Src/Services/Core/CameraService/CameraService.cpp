#include "Services/Core/CameraService/CameraService.h"

#include "Log/LogManager.h"
#include "Services/Core/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "ThreadPool/ThreadPool.h"
#include "Utils/FileNameSanitizer.h"
#include "Utils/Sanitize.h"
#include "core.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace
{
const std::string PATHS_DIR = "camerapaths";

constexpr Milliseconds SEGMENT_TIME_MIN{200};
constexpr Milliseconds SEGMENT_TIME_MAX{120000};
} // namespace

// ------------------------------------------------------------------ парсинг и реестр

bool CameraService::parsePath(const std::string &content, CameraPath &out)
{
    CameraPath path;

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line) && path.points.size() < MAX_POINTS)
    {
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;

        if (kind == "settings")
        {
            long long timeMs = 0;
            int loopFlag = 0;
            if (ss >> timeMs >> loopFlag)
            {
                path.segmentTime = std::clamp(Milliseconds(timeMs), SEGMENT_TIME_MIN, SEGMENT_TIME_MAX);
                path.loop = loopFlag != 0;
            }
        }
        else if (kind == "point")
        {
            CameraPathPoint p;
            if (ss >> p.position.x >> p.position.y >> p.position.z >> p.lookAt.x >> p.lookAt.y >> p.lookAt.z)
            {
                p.position = Utils::sanitize(p.position);
                p.lookAt = Utils::sanitize(p.lookAt);
                path.points.push_back(p);
            }
        }
    }

    if (path.points.empty())
    {
        return false;
    }
    out = std::move(path);
    return true;
}

void CameraService::registerPath(const std::string &name, CameraPath path)
{
    m_paths[name] = sanitizePath(std::move(path));
}

bool CameraService::hasPath(const std::string &name) const
{
    return m_paths.find(name) != m_paths.end();
}

void CameraService::loadPathFromFile(const std::string &name, LoadHandler onLoaded)
{
    if (!validFileName(name))
    {
        LogManager::log(Error, "CameraService: invalid path name '" + name + "'");
        if (onLoaded)
        {
            onLoaded(false);
        }
        return;
    }

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
    // Общий handler на оба колбэка: callback и errorCallback — взаимоисключающие продолжения
    // одной задачи, но выполняются из разных лямбд, поэтому onLoaded нельзя move-нуть в одну
    // из них (иначе вторая получит опустошённый std::function).
    auto handler = std::make_shared<LoadHandler>(std::move(onLoaded));
    task.callback = [this, name, handler](std::string content)
    {
        CameraPath parsed;
        const bool ok = parsePath(content, parsed);
        if (ok)
        {
            m_paths[name] = std::move(parsed);
        }
        else
        {
            LogManager::log(Warning, "CameraService: no valid points in camerapaths/" + name + ".txt");
        }
        if (*handler)
        {
            (*handler)(ok);
        }
    };
    task.errorCallback = [name, handler](const std::string &error)
    {
        LogManager::log(Warning, "CameraService: failed to load path '" + name + "': " + error);
        if (*handler)
        {
            (*handler)(false);
        }
    };
    ThreadPool::addTask(std::move(task));
}

// ------------------------------------------------------------------ проигрывание

bool CameraService::play(IPlayer &player, const CameraPath &path, FinishHandler onFinish)
{
    if (!m_timers)
    {
        LogManager::log(Error, "CameraService: not initialized");
        return false;
    }

    CameraPath clean = sanitizePath(path);
    if (clean.points.size() < 2)
    {
        return false;
    }

    Slot &slot = m_slots[player.getID()];
    m_timers->cancel(slot.timer); // перезапуск заменяет текущее проигрывание (без onFinish)

    slot.playing = true;
    slot.path = std::move(clean);
    slot.segment = 0;
    slot.onFinish = std::move(onFinish);

    interpolateSegment(player, slot.path.points[0], slot.path.points[1], slot.path.segmentTime);
    scheduleAdvance(player, slot);
    return true;
}

bool CameraService::playFromFile(IPlayer &player, const std::string &name, FinishHandler onFinish)
{
    auto it = m_paths.find(name);
    if (it != m_paths.end())
    {
        return play(player, it->second, std::move(onFinish));
    }

    if (!validFileName(name))
    {
        return false;
    }

    // Ленивая загрузка: за время чтения файла игрок мог выйти, а слот занять
    // другой — сверяем версию подключения.
    const int version = m_versions ? m_versions->getVersion(player.getID()) : 0;
    loadPathFromFile(name,
                     [this, playerId = player.getID(), version, name, onFinish = std::move(onFinish)](bool ok) mutable
                     {
                         if (!ok)
                         {
                             return;
                         }
                         IPlayer *player = m_core ? m_core->getPlayers().get(playerId) : nullptr;
                         if (!player || (m_versions && m_versions->getVersion(playerId) != version))
                         {
                             return;
                         }
                         auto it = m_paths.find(name);
                         if (it != m_paths.end())
                         {
                             play(*player, it->second, std::move(onFinish));
                         }
                     });
    return true;
}

void CameraService::stop(IPlayer &player, bool restoreCamera)
{
    Slot &slot = m_slots[player.getID()];
    m_timers->cancel(slot.timer);
    slot.playing = false;
    slot.onFinish = nullptr;
    slot.path = CameraPath{};

    if (restoreCamera)
    {
        player.setCameraBehind();
    }
}

bool CameraService::isPlaying(int playerId) const
{
    if (!validPlayerId(playerId))
        return false;
    return m_slots[playerId].playing;
}

int CameraService::segmentIndex(int playerId) const
{
    if (!validPlayerId(playerId))
        return -1;
    return m_slots[playerId].segment;
}

// ------------------------------------------------------------------ вызовы CameraSystem

void CameraService::initialize(ICore *core, TimerService *timers, PlayerConnectionVersionService *versions)
{
    m_core = core;
    m_timers = timers;
    m_versions = versions;
}

void CameraService::resetPlayer(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    // Пер-плеерный таймер TimerService уже погасил при дисконнекте.
    m_slots[playerId] = Slot{};
}

// ------------------------------------------------------------------ private

void CameraService::advance(IPlayer &player)
{
    Slot &slot = m_slots[player.getID()];
    const int n = static_cast<int>(slot.path.points.size());
    if (!slot.playing || n < 2)
    {
        return;
    }

    // Сегмент slot.segment закончился: камера в points[(segment + 1) % n].
    const int arrived = (slot.segment + 1) % n;

    if (!slot.path.loop && arrived >= n - 1)
    {
        slot.playing = false;
        slot.timer = {};
        slot.path = CameraPath{};
        // Забираем колбэк до вызова: он может тут же запустить новый путь.
        FinishHandler finish = std::move(slot.onFinish);
        slot.onFinish = nullptr;
        if (finish)
        {
            finish(player);
        }
        return;
    }

    slot.segment = arrived;
    interpolateSegment(player, slot.path.points[arrived], slot.path.points[(arrived + 1) % n], slot.path.segmentTime);
    scheduleAdvance(player, slot);
}

void CameraService::scheduleAdvance(IPlayer &player, Slot &slot)
{
    slot.timer = m_timers->setPlayerTimeout(player, slot.path.segmentTime, [this](IPlayer &p) { advance(p); });
}

void CameraService::interpolateSegment(IPlayer &player, const CameraPathPoint &from, const CameraPathPoint &to,
                                       Milliseconds time)
{
    const int timeMs = static_cast<int>(time.count());
    player.interpolateCameraPosition(from.position, to.position, timeMs, PlayerCameraCutType_Move);
    player.interpolateCameraLookAt(from.lookAt, to.lookAt, timeMs, PlayerCameraCutType_Move);
}

CameraPath CameraService::sanitizePath(CameraPath path)
{
    if (path.points.size() > MAX_POINTS)
    {
        path.points.resize(MAX_POINTS);
    }
    for (CameraPathPoint &point : path.points)
    {
        point.position = Utils::sanitize(point.position);
        point.lookAt = Utils::sanitize(point.lookAt);
    }
    path.segmentTime = std::clamp(path.segmentTime, SEGMENT_TIME_MIN, SEGMENT_TIME_MAX);
    return path;
}

bool CameraService::validFileName(const std::string &name)
{
    return Utils::isValidPresetName(name); // общий whitelist (закрывает и ':' ADS-gap)
}
