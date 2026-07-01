#include "Services/Core/PickupService/PickupService.h"

#include "Log/LogManager.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/StreamerService/StreamerService.h"
#include <algorithm>
#include <cmath>
#include <fmt/format.h>

namespace
{
constexpr int MAX_PICKUP_MODEL = 19999;
constexpr int MAX_PICKUP_TYPE = 23; // клиентские типы поведения SA: 0..23
} // namespace

int PickupService::add(int model, PickupType type, const Vector3 &position, Handler onPickUp,
                       std::uint32_t virtualWorld, Milliseconds rearmGap)
{
    if (!m_streamer)
    {
        LogManager::log(Error, "PickupService: not initialized, pickup dropped");
        return -1;
    }

    model = std::clamp(model, 0, MAX_PICKUP_MODEL);
    type = static_cast<PickupType>(std::clamp<int>(type, 0, MAX_PICKUP_TYPE));
    rearmGap = std::max(rearmGap, Milliseconds(0));

    const int pickupId = m_streamer->addPickup(model, type, position, virtualWorld);
    if (pickupId < 0)
    {
        LogManager::log(Warning, "PickupService: streamer rejected pickup");
        return -1;
    }

    Def def;
    def.position = position;
    def.virtualWorld = virtualWorld;
    def.rearmGap = rearmGap;
    def.model = model;
    def.handler = std::move(onPickUp);
    m_defs[pickupId] = std::move(def);

    return pickupId;
}

void PickupService::remove(int pickupId)
{
    if (m_defs.erase(pickupId) > 0 && m_streamer)
    {
        m_streamer->removePickup(pickupId);
    }
}

bool PickupService::exists(int pickupId) const
{
    return m_defs.find(pickupId) != m_defs.end();
}

// ------------------------------------------------------------------ вызовы PickupSystem

void PickupService::initialize(StreamerService *streamer, PlayerLocationService *location,
                               AntiCheatService *antiCheat)
{
    m_streamer = streamer;
    m_location = location;
    m_antiCheat = antiCheat;
}

void PickupService::handlePickUp(IPlayer &player, IPickup &pickup, TimePoint now)
{
    if (!m_streamer || !m_location)
    {
        return;
    }

    const int pickupId = m_streamer->pickupDefByPoolId(pickup.getID());
    if (pickupId < 0)
    {
        return; // пикап не из стримера — не наш
    }

    auto it = m_defs.find(pickupId);
    if (it == m_defs.end())
    {
        return; // пикап стримера без обработчика (декорация)
    }
    const Def &def = it->second;
    const int playerId = player.getID();

    // Виртуальный мир: пикап из чужого мира клиенту не стримится — заявление
    // о его подборе возможно только подделкой RPC.
    if (static_cast<std::uint32_t>(m_location->getVirtualWorld(playerId)) != def.virtualWorld)
    {
        if (m_antiCheat)
        {
            m_antiCheat->record(playerId, AntiCheatService::ViolationType::PickupHack,
                                fmt::format("pickup {} (model {}): player vw {} != pickup vw {}", pickupId, def.model,
                                            m_location->getVirtualWorld(playerId), def.virtualWorld),
                                now);
        }
        return;
    }

    // Дистанция: считаем от принятой сервером позиции, а не от заявления клиента.
    const Vector3 playerPos = m_location->getPosition(playerId);
    const Vector3 delta = playerPos - def.position;
    const float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    if (distSq > MAX_PICKUP_DISTANCE * MAX_PICKUP_DISTANCE)
    {
        if (m_antiCheat)
        {
            m_antiCheat->record(playerId, AntiCheatService::ViolationType::PickupHack,
                                fmt::format("pickup {} (model {}): distance {:.1f}m > {:.1f}m", pickupId, def.model,
                                            std::sqrt(distSq), MAX_PICKUP_DISTANCE),
                                now);
        }
        return;
    }

    // Edge-триггер: клиент шлёт событие подбора ПОВТОРНО, пока игрок стоит на
    // пикапе (open.mp не троттлит — чистый релей RPC). Держим время последнего
    // ПРИНЯТОГО события; handler зовём только на СВЕЖЕМ входе — когда прошлого
    // события не было вовсе, либо поток прерывался дольше rearmGap (игрок сходил
    // с пикапа). Время события обновляем ВСЕГДА (валидное событие продлевает
    // «игрок на пикапе»), решение о вызове — до обновления.
    auto &lastByDef = m_lastPickUp[playerId];
    auto lastIt = lastByDef.find(pickupId);
    const bool freshEntry = lastIt == lastByDef.end() || now - lastIt->second > def.rearmGap;
    lastByDef[pickupId] = now;
    if (!freshEntry)
    {
        return;
    }

    // Копия на случай, если обработчик удалит свой же пикап (remove сотрёт def
    // вместе с std::function — оригинал нельзя разрушать во время вызова).
    Handler handler = def.handler;
    if (handler)
    {
        handler(player);
    }
}

void PickupService::resetPlayer(int playerId)
{
    m_lastPickUp[playerId].clear();
}
