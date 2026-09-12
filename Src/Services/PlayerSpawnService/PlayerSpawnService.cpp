#include "Services/PlayerSpawnService/PlayerSpawnService.h"

#include "Log/LogManager.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerSkinService/PlayerSkinService.h"
#include <algorithm>
#include <cmath>

namespace
{
constexpr float WORLD_MIN = -20000.0f; // интерьеры лежат далеко за картой — кламп щадящий
constexpr float WORLD_MAX = 20000.0f;
constexpr int MAX_INTERIOR = 255;
constexpr int NO_TEAM = 255;

float clampCoord(float value)
{
    return std::clamp(std::isfinite(value) ? value : 0.0f, WORLD_MIN, WORLD_MAX);
}
} // namespace

void PlayerSpawnService::setSpawn(IPlayer &player, const SpawnPoint &point)
{
    const int id = player.getID();
    if (id < 0 || id >= MAX_PLAYERS)
    {
        return;
    }
    Slot &slot = m_slots[id];
    slot.customized = true;
    slot.point = sanitize(point);
    applySpawnInfo(player);
}

const SpawnPoint &PlayerSpawnService::getSpawn(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return m_defaultSpawn;
    }
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

void PlayerSpawnService::refreshSpawnInfo(IPlayer &player)
{
    applySpawnInfo(player);
}

// ------------------------------------------------------------------ вызовы PlayerSpawnSystem

void PlayerSpawnService::initialize(PlayerLocationService *location, PlayerSkinService *skins)
{
    m_location = location;
    m_skins = skins;
    // Спавн-инфо (скин в class-данных) обязано совпадать с БАЗОЙ всегда: нативный
    // респаун после смерти берёт скин ИМЕННО отсюда, и его никто не пересобирает
    // между смертью и авто-респауном. Поэтому при ЛЮБОЙ смене базы (вступление во
    // фракцию, /skin, /devskin вне фракции) пересобираем спавн-инфо — иначе первая
    // же смерть откатила бы игрока на прежнюю базу. Порядок вызовов у источников
    // смены базы при этом перестаёт быть хрупким.
    if (m_skins)
    {
        m_skins->subscribeBaseChange([this](IPlayer &player) { applySpawnInfo(player); });
    }
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
    // Интерьер и виртуальный мир в class-данные не входят — докатываем здесь.
    // ПОЗИЦИЮ тоже принудительно: клиент спавнится по class-данным
    // (SetSpawnInfo — отдельный RPC), и при смене точки прямо перед спавном
    // (членство фракции пришло из БД на логине) он использует СТАРУЮ позицию
    // с НОВЫМ интерьером — «в небе без стен». Серверный телепорт в точку
    // правды закрывает гонку при любом порядке доставки.
    const SpawnPoint &point = getSpawn(player.getID());
    if (m_location)
    {
        m_location->setInterior(player, point.interior);
        m_location->setVirtualWorld(player, point.virtualWorld);
        m_location->teleport(player, point.position);
        player.setRotation(GTAQuat(Vector3(0.0f, 0.0f, point.angle)));
    }
    // Камера всегда за спиной: после смерти/телепорта клиент любит оставлять
    // её где попало — игрок спавнится, глядя в случайную сторону.
    player.setCameraBehind();
}

void PlayerSpawnService::resetPlayer(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
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
    // Скин — из его источника правды; оружие на спавне не выдаём — экипировка
    // остаётся за бизнес-системами (иначе спавн-инфо станет вторым источником
    // правды об инвентаре).
    const int skin = m_skins ? m_skins->getSkin(player.getID()) : PlayerSkinService::DEFAULT_SKIN;
    data->setSpawnInfo(PlayerClass(skin, NO_TEAM, point.position, point.angle, WeaponSlots{}));
}

SpawnPoint PlayerSpawnService::sanitize(SpawnPoint point)
{
    point.position = {clampCoord(point.position.x), clampCoord(point.position.y), clampCoord(point.position.z)};
    point.angle = std::isfinite(point.angle) ? point.angle : 0.0f;
    point.interior = std::min(point.interior, static_cast<unsigned>(MAX_INTERIOR));
    return point;
}
