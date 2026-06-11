#pragma once

#include "Services/Core/PickupService/PickupService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Pickups/pickups.hpp>

// Единственный подписчик на события пикапов: маршрутизирует подбор в
// PickupService (там валидация и обработчики) и чистит per-player состояние
// при отключении.
class PickupSystem : public BaseSystem, public PickupEventHandler, public PlayerConnectEventHandler
{
  public:
    PickupSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerPickUpPickup(IPlayer &player, IPickup &pickup) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    PickupService &m_pickupService;
};
