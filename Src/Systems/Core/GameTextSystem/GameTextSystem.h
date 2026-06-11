#pragma once

#include "Services/Core/GameTextService/GameTextService.h"
#include "Systems/BaseSystem.h"

// Проводник GameTextService: только связывает сервис с ядром — per-player
// состояния у gametext нет (его ведёт сам open.mp).
class GameTextSystem : public BaseSystem
{
  public:
    GameTextSystem(ICore &core, const ServiceRegister &serviceRegister);
};
