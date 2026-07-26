#include "Services/Core/CheckpointService/CheckpointService.h"

#include "Log/LogManager.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Utils/Sanitize.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fmt/format.h>

namespace
{
constexpr auto STREAM_INTERVAL = std::chrono::milliseconds(500);
// Пауза между Disable и повторным Set: клиент пересоздаёт маркер чекпоинта
// (и применяет новый размер/высоту) только если они пришли в разных кадрах.
constexpr auto RESHOW_DELAY = std::chrono::milliseconds(100);
// Минимум между срабатываниями onEnter для одного игрока: дрожание позиции на
// границе чекпоинта (в т.ч. умышленное) не спамит обработчики.
constexpr auto ENTER_DEBOUNCE = std::chrono::milliseconds(500);

float distanceSq(const Vector3 &a, const Vector3 &b)
{
    const Vector3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}
} // namespace

// ------------------------------------------------------------------ глобальные

int CheckpointService::add(const Vector3 &position, float radius, EnterHandler onEnter, LeaveHandler onLeave)
{
    Def def;
    def.position = Utils::sanitize(position);
    def.radius = clampRadius(radius);
    def.onEnter = std::move(onEnter);
    def.onLeave = std::move(onLeave);

    const int id = m_nextId++;
    m_defs[id] = std::move(def);
    return id;
}

void CheckpointService::remove(int checkpointId)
{
    m_defs.erase(checkpointId);
    // Игрокам, которым он показан, чекпоинт уберёт ближайший проход стрима;
    // запоздалые enter-события отфильтрует handleEnter по отсутствию def.
}

bool CheckpointService::exists(int checkpointId) const
{
    return m_defs.find(checkpointId) != m_defs.end();
}

bool CheckpointService::update(int checkpointId, const Vector3 &position, float radius)
{
    auto it = m_defs.find(checkpointId);
    if (it == m_defs.end())
    {
        return false;
    }
    it->second.position = Utils::sanitize(position);
    it->second.radius = clampRadius(radius);

    // Сбрасываем показ у тех, кому он показан: ближайший проход стрима
    // перепокажет с новыми параметрами.
    for (Slot &slot : m_slots)
    {
        if (slot.shownDef == checkpointId)
        {
            slot.shownDef = -1;
            slot.nextStreamAt = TimePoint{};
        }
    }
    return true;
}

// ------------------------------------------------------------------ персональный

void CheckpointService::setForPlayer(IPlayer &player, const Vector3 &position, float radius, EnterHandler onEnter,
                                     LeaveHandler onLeave)
{
    Slot &slot = m_slots[player.getID()];
    slot.personal = true;
    slot.personalPosition = Utils::sanitize(position);
    slot.personalRadius = clampRadius(radius);
    slot.personalEnter = std::move(onEnter);
    slot.personalLeave = std::move(onLeave);
    slot.shownDef = -1;

    showCheckpoint(player, slot.personalPosition, slot.personalRadius);
}

void CheckpointService::clearForPlayer(IPlayer &player)
{
    Slot &slot = m_slots[player.getID()];
    if (!slot.personal)
    {
        return;
    }
    slot.personal = false;
    slot.personalEnter = nullptr;
    slot.personalLeave = nullptr;
    hideCheckpoint(player);
    slot.nextStreamAt = TimePoint{}; // ближайший проход стрима покажет глобальный
}

bool CheckpointService::hasPersonal(int playerId) const
{
    if (!validPlayerId(playerId))
        return false;
    return m_slots[playerId].personal;
}

// ------------------------------------------------------------------ гоночный

void CheckpointService::setRaceForPlayer(IPlayer &player, RaceCheckpointType type, const Vector3 &position,
                                         const Vector3 &nextPosition, float radius, EnterHandler onEnter,
                                         LeaveHandler onLeave)
{
    IPlayerCheckpointData *data = queryExtension<IPlayerCheckpointData>(player);
    if (!data)
    {
        LogManager::log(Error, "CheckpointService: IPlayerCheckpointData extension is missing");
        return;
    }

    if (type < RaceCheckpointType::RACE_NORMAL || type > RaceCheckpointType::RACE_NONE)
    {
        type = RaceCheckpointType::RACE_NORMAL;
    }

    Slot &slot = m_slots[player.getID()];
    slot.race = true;
    slot.racePosition = Utils::sanitize(position);
    slot.raceRadius = clampRadius(radius);
    slot.raceEnter = std::move(onEnter);
    slot.raceLeave = std::move(onLeave);

    IRaceCheckpointData &race = data->getRaceCheckpoint();
    // Как и у обычного: обновление параметров требует disable() перед enable().
    if (race.isEnabled())
    {
        race.disable();
    }
    race.setType(type);
    race.setPosition(slot.racePosition);
    race.setNextPosition(Utils::sanitize(nextPosition));
    race.setRadius(slot.raceRadius);
    race.enable();
}

void CheckpointService::clearRaceForPlayer(IPlayer &player)
{
    Slot &slot = m_slots[player.getID()];
    if (!slot.race)
    {
        return;
    }
    slot.race = false;
    slot.raceEnter = nullptr;
    slot.raceLeave = nullptr;

    if (IPlayerCheckpointData *data = queryExtension<IPlayerCheckpointData>(player))
    {
        data->getRaceCheckpoint().disable();
    }
}

// ------------------------------------------------------------------ вызовы CheckpointSystem

void CheckpointService::initialize(PlayerLocationService *location, AntiCheatService *antiCheat)
{
    m_location = location;
    m_antiCheat = antiCheat;
}

void CheckpointService::streamPlayer(IPlayer &player, const Vector3 &position, TimePoint now)
{
    Slot &slot = m_slots[player.getID()];

    // Отложенный показ (вторая фаза пересоздания маркера) — вне троттла стрима.
    if (slot.pendingShow && now >= slot.pendingAt)
    {
        slot.pendingShow = false;
        showCheckpoint(player, slot.pendingPosition, slot.pendingRadius); // чекпоинт скрыт — покажется сразу
    }

    if (now < slot.nextStreamAt)
    {
        return;
    }
    slot.nextStreamAt = now + STREAM_INTERVAL;

    if (slot.personal)
    {
        return; // личный чекпоинт приоритетнее — глобальные не показываем
    }

    int best = -1;
    float bestSq = STREAM_DISTANCE * STREAM_DISTANCE;
    for (const auto &[id, def] : m_defs)
    {
        const float distSq = distanceSq(position, def.position);
        if (distSq < bestSq)
        {
            bestSq = distSq;
            best = id;
        }
    }

    if (best == slot.shownDef)
    {
        return;
    }

    slot.shownDef = best;
    if (best < 0)
    {
        hideCheckpoint(player);
    }
    else
    {
        const Def &def = m_defs[best];
        showCheckpoint(player, def.position, def.radius);
    }
}

void CheckpointService::handleEnter(IPlayer &player, TimePoint now)
{
    Slot &slot = m_slots[player.getID()];

    // Дрожание на границе (в т.ч. умышленное читом) даёт серию enter/leave —
    // обработчик зовём не чаще раза в ENTER_DEBOUNCE.
    if (now - slot.lastEnterAt < ENTER_DEBOUNCE)
    {
        return;
    }

    if (slot.personal)
    {
        if (!validateInside(player, slot.personalPosition, slot.personalRadius, now))
        {
            return;
        }
        slot.lastEnterAt = now;
        // Копия: обработчик может сменить/снять чекпоинт, разрушив оригинал.
        EnterHandler handler = slot.personalEnter;
        if (handler)
        {
            handler(player);
        }
        return;
    }

    if (slot.shownDef < 0)
    {
        return; // сервер ничего не показывал — событие не к нашему чекпоинту
    }
    auto it = m_defs.find(slot.shownDef);
    if (it == m_defs.end())
    {
        return; // чекпоинт удалили, клиент ещё не узнал
    }
    if (!validateInside(player, it->second.position, it->second.radius, now))
    {
        return;
    }

    slot.lastEnterAt = now;
    EnterHandler handler = it->second.onEnter;
    if (handler)
    {
        handler(player);
    }
}

void CheckpointService::handleLeave(IPlayer &player)
{
    Slot &slot = m_slots[player.getID()];

    if (slot.personal)
    {
        LeaveHandler handler = slot.personalLeave;
        if (handler)
        {
            handler(player);
        }
        return;
    }

    if (slot.shownDef >= 0)
    {
        auto it = m_defs.find(slot.shownDef);
        if (it != m_defs.end() && it->second.onLeave)
        {
            LeaveHandler handler = it->second.onLeave;
            handler(player);
        }
    }
}

void CheckpointService::handleRaceEnter(IPlayer &player, TimePoint now)
{
    Slot &slot = m_slots[player.getID()];
    if (!slot.race)
    {
        return;
    }
    if (now - slot.lastRaceEnterAt < ENTER_DEBOUNCE)
    {
        return;
    }
    if (!validateInside(player, slot.racePosition, slot.raceRadius, now))
    {
        return;
    }
    slot.lastRaceEnterAt = now;
    EnterHandler handler = slot.raceEnter;
    if (handler)
    {
        handler(player);
    }
}

void CheckpointService::handleRaceLeave(IPlayer &player)
{
    Slot &slot = m_slots[player.getID()];
    if (!slot.race)
    {
        return;
    }
    LeaveHandler handler = slot.raceLeave;
    if (handler)
    {
        handler(player);
    }
}

void CheckpointService::resetPlayer(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    m_slots[playerId] = Slot{};
}

// ------------------------------------------------------------------ private

void CheckpointService::showCheckpoint(IPlayer &player, const Vector3 &position, float radius)
{
    IPlayerCheckpointData *data = queryExtension<IPlayerCheckpointData>(player);
    if (!data)
    {
        LogManager::log(Error, "CheckpointService: IPlayerCheckpointData extension is missing");
        return;
    }
    ICheckpointData &checkpoint = data->getCheckpoint();
    Slot &slot = m_slots[player.getID()];

    if (checkpoint.isEnabled())
    {
        // Маркер уже показан: Disable и Set в одном кадре клиент склеивает —
        // маркер не пересоздаётся, и новый размер/высота не применяются
        // (позиция при этом обновилась бы — клиент читает её каждый кадр).
        // Прячем сейчас, показываем отложенно из streamPlayer.
        checkpoint.disable();
        slot.pendingShow = true;
        slot.pendingPosition = position;
        slot.pendingRadius = radius;
        slot.pendingAt = Time::now() + RESHOW_DELAY;
        return;
    }

    slot.pendingShow = false;
    checkpoint.setPosition(position);
    checkpoint.setRadius(radius);
    checkpoint.enable();
}

void CheckpointService::hideCheckpoint(IPlayer &player)
{
    m_slots[player.getID()].pendingShow = false;
    if (IPlayerCheckpointData *data = queryExtension<IPlayerCheckpointData>(player))
    {
        data->getCheckpoint().disable();
    }
}

bool CheckpointService::validateInside(IPlayer &player, const Vector3 &position, float radius, TimePoint now)
{
    if (!m_location)
    {
        return true;
    }

    const Vector3 accepted = m_location->getPosition(player.getID());
    const float maxDist = radius + ENTER_SLACK;
    const float distSq = distanceSq(accepted, position);
    if (distSq <= maxDist * maxDist)
    {
        return true;
    }

    if (m_antiCheat)
    {
        m_antiCheat->record(player.getID(), AntiCheatService::ViolationType::CheckpointHack,
                            fmt::format("checkpoint enter: distance {:.1f}m > {:.1f}m (radius {:.1f})",
                                        std::sqrt(distSq), maxDist, radius),
                            now);
    }
    return false;
}

float CheckpointService::clampRadius(float radius)
{
    return std::clamp(std::isfinite(radius) ? radius : MIN_RADIUS, MIN_RADIUS, MAX_RADIUS);
}
