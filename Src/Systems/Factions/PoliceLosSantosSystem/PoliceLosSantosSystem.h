#pragma once

#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Полиция Лос-Сантоса. Курируется Администрацией
// Президента (лидера назначает министр через /gov). База — участок LSPD
// (интерьер 6), два входа с улицы и два выхода. Геймплейные биты маски
// (розыск, склад, ...) добавятся вместе с механиками.
class PoliceLosSantosSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 2;

    PoliceLosSantosSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    FactionService &m_factionService;
};
