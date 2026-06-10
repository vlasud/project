#pragma once

#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include "types.hpp"
#include <Server/Components/Classes/classes.hpp>
#include <Server/Components/Objects/objects.hpp>

class DebugCameraSystem : public BaseSystem, public PlayerChangeEventHandler, public PlayerUpdateEventHandler
{
  public:
    DebugCameraSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;

  private:
    bool m_isEnabled = false;
    Vector3 m_cameraPosition{};
    Vector3 m_cameraDirection{};

    IObjectsComponent *m_component = nullptr;
    IObject *m_object = nullptr;

    PlayerCommandService &m_commandService;
    PlayerAnimationService &m_animationService;
    PlayerLocationService &m_locationService;
};
