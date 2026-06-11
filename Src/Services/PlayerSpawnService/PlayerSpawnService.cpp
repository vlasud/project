#include "Services/PlayerSpawnService/PlayerSpawnService.h"

#include "Log/LogManager.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include <algorithm>
#include <cmath>

namespace
{
constexpr float WORLD_MIN = -20000.0f; // интерьеры лежат далеко за картой — кламп щадящий
constexpr float WORLD_MAX = 20000.0f;
constexpr int MAX_SKIN = 311;
constexpr int MAX_INTERIOR = 255;
constexpr int NO_TEAM = 255;

float clampCoord(float value)
{
    return std::clamp(std::isfinite(value) ? value : 0.0f, WORLD_MIN, WORLD_MAX);
}
} // namespace

void PlayerSpawnService::setSpawn(IPlayer &player, const SpawnPoint &point)
{
    Slot &slot = m_slots[player.getID()];
    slot.customized = true;
    slot.point = sanitize(point);
    applySpawnInfo(player);
}

const SpawnPoint &PlayerSpawnService::getSpawn(int playerId) const
{
    const Slot &slot = m_slots[playerId];
    return slot.customized ? slot.point : m_defaultSpawn;
}

void PlayerSpawnService::setDefaultSpawn(const SpawnPoint &point)
{
    // Действует на тех, кому setSpawn не вызывали: новые подключения сразу,
    // онлайн-игроки — при следующем applySpawnInfo (setSpawn/respawn).
    m_defaultSpawn = sanitize(point);
}

void PlayerSpawnService::respawn(IPlayer &player)
{
    applySpawnInfo(player); // на случай смены дефолта после коннекта
    player.spawn();
}

// ------------------------------------------------------------------ вызовы PlayerSpawnSystem

void PlayerSpawnService::initialize(PlayerLocationService *location)
{
    m_location = location;
}

void PlayerSpawnService::handleConnect(IPlayer &player)
{
    // Только сброс: расширение IPlayerClassData в момент коннекта ещё не
    // создано компонентом классов. Спавн-инфо уйдёт при первом respawn()/
    // setSpawn() — раньше первого спавна оно клиенту не нужно.
    m_slots[player.getID()] = Slot{};
}

void PlayerSpawnService::handleSpawn(IPlayer &player)
{
    // Позицию/угол/скин клиент применил сам из class-данных; интерьер и
    // виртуальный мир в них не входят — докатываем здесь.
    const SpawnPoint &point = getSpawn(player.getID());
    if (m_location)
    {
        m_location->setInterior(player, point.interior);
        m_location->setVirtualWorld(player, point.virtualWorld);
    }
}

void PlayerSpawnService::resetPlayer(int playerId)
{
    m_slots[playerId] = Slot{};
}

// ------------------------------------------------------------------ private

void PlayerSpawnService::applySpawnInfo(IPlayer &player)
{
    IPlayerClassData *data = queryExtension<IPlayerClassData>(player);
    if (!data)
    {
        LogManager::log(Error, "PlayerSpawnService: IPlayerClassData extension is missing");
        return;
    }

    const SpawnPoint &point = getSpawn(player.getID());
    // Оружие на спавне не выдаём — экипировка остаётся за бизнес-системами
    // (иначе спавн-инфо станет вторым источником правды об инвентаре).
    data->setSpawnInfo(PlayerClass(point.skin, NO_TEAM, point.position, point.angle, WeaponSlots{}));
}

SpawnPoint PlayerSpawnService::sanitize(SpawnPoint point)
{
    point.position = {clampCoord(point.position.x), clampCoord(point.position.y), clampCoord(point.position.z)};
    point.angle = std::isfinite(point.angle) ? point.angle : 0.0f;
    point.skin = std::clamp(point.skin, 0, MAX_SKIN);
    point.interior = std::min(point.interior, static_cast<unsigned>(MAX_INTERIOR));
    return point;
}
