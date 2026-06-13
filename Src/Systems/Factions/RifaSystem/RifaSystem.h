#pragma once

#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Rifa — криминальная организация (как Триада, БЕЗ
// кураторства Администрации Президента: гос-вертикаль /gov ей не управляет,
// лидер — через /fdev). База — интерьер 17, вход/выход на улице Сан-Фиерро;
// изоляция по виртуальному миру (= id фракции).
class RifaSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 10; // 1 — АП, 2-4 — полиции, 5-7 — банки, 8 — ФБР, 9 — Триада

    RifaSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    FactionService &m_factionService;
};
