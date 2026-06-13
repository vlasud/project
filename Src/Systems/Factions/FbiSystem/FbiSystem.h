#pragma once

#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: ФБР. Курируется Администрацией Президента (лидера
// назначает министр через /gov). Своя точка входа/выхода на улице Сан-Фиерро,
// а интерьер и внутренние позиции — общие с участком SFPD (интерьер 10):
// изоляция по виртуальному миру (= id фракции), поэтому ФБР и полиция СФ делят
// интерьер, не видя друг друга.
class FbiSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 8; // 1 — АП, 2-4 — полиции, 5-7 — банки

    FbiSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    FactionService &m_factionService;
};
