#pragma once

#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Итальянская мафия — криминальная организация. Как
// Триада/Rifa НЕ под кураторством Администрации Президента (supervisorId не
// задаётся): гос-вертикаль (/gov) ей не управляет, лидер назначается дев-меню
// (/fdev), а в будущем — собственной криминальной механикой. База — интерьер 1
// (Caligula's Casino), вход/выход на улице Лас-Вентураса; изоляция по
// виртуальному миру (= id фракции).
class ItalianMafiaSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 18; // 1 — АП, 2-4 — полиции, 5-7 — банки, 8 — ФБР, 9-17 — банды/армия/РМ

    ItalianMafiaSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    FactionService &m_factionService;
};
