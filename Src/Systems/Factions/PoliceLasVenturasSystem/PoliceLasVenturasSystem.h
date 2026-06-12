#pragma once

#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Полиция Лас-Вентураса. Курируется Администрацией
// Президента (лидера назначает министр через /gov). База — участок LVPD
// (интерьер 3): входы с улицы и с гаража, выход на улицу.
class PoliceLasVenturasSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 4;

    PoliceLasVenturasSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    FactionService &m_factionService;
};
