#pragma once

#include "Services/Core/TextLabelService/TextLabelService.h"
#include "Systems/BaseSystem.h"

// Связывает TextLabelService со стримером. Per-player состояние лейблов ведёт
// сам стример, событий у лейблов нет — системе остаётся только проводка.
class TextLabelSystem : public BaseSystem
{
  public:
    TextLabelSystem(ICore &core, const ServiceRegister &serviceRegister);
};
