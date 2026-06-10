#include "DebugCameraSystem.h"
#include "glm/geometric.hpp"
#include "types.hpp"

DebugCameraSystem::DebugCameraSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_commandService(serviceRegister.getService<PlayerCommandService>()),
      m_animationService(serviceRegister.getService<PlayerAnimationService>())
{
    core.getPlayers().getPlayerChangeDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);

    m_commandService.add("camera", {},
                         [this](IPlayer &player, ...)
                         {
                             m_isEnabled = !m_isEnabled;

                             if (m_isEnabled)
                             {
                                 m_cameraPosition = player.getPosition();
                                 m_cameraDirection = player.getCameraLookAt();

                                 m_object = m_component->create(0, m_cameraPosition, m_cameraDirection);
                                 player.attachCameraToObject(*m_object);
                             }
                             else
                             {
                                 m_component->release(m_object->getID());
                                 m_object = nullptr;
                                 player.setCameraBehind();
                             }
                         });
}

void DebugCameraSystem::initialize(IComponentList *components)
{
    m_component = components->queryComponent<IObjectsComponent>();
}

bool DebugCameraSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    if (!m_isEnabled)
    {
        return true;
    }

    const PlayerAimData &aimData = player.getAimData();
    const Vector3 forward = glm::normalize(aimData.camFrontVector);

    constexpr float SPEED = 1.0f;

    const PlayerKeyData &keyData = player.getKeyData();

    if (keyData.upDown)
    {
        m_cameraPosition += forward * (keyData.upDown > 0 ? -SPEED : SPEED);
    }

    if (keyData.leftRight)
    {
        Vector3 right = glm::vec3(forward.y, -forward.x, 0.0f);
        const float len = glm::length(right);
        if (len > 0.0001f)
        {
            right /= len;
            m_cameraPosition += right * (keyData.leftRight > 0 ? SPEED : -SPEED);
        }
    }

    m_object->setPosition(m_cameraPosition);

    return true;
}
