#pragma once

#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Полиция Сан-Фиерро. Курируется Администрацией
// Президента (лидера назначает министр через /gov). База — участок SFPD
// (интерьер 10): входы с улицы и из гаража, выходы на улицу и в гараж.
class PoliceSanFierroSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 3;

    PoliceSanFierroSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    FactionService &m_factionService;
};
