#pragma once

#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Триада — криминальная организация. В отличие от полиций /
// ФБР / банков НЕ под кураторством Администрации Президента (supervisorId не
// задаётся): гос-вертикаль (/gov) ей не управляет, лидер назначается дев-меню
// (/fdev), а в будущем — собственной криминальной механикой. База — интерьер 1,
// вход/выход на улице Сан-Фиерро; изоляция по виртуальному миру (= id фракции).
class TriadSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 9; // 1 — АП, 2-4 — полиции, 5-7 — банки, 8 — ФБР

    TriadSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    FactionService &m_factionService;
};
