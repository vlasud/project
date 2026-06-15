#pragma once

#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Русская мафия — криминальная организация. Как Триада/Rifa
// НЕ под кураторством Администрации Президента (supervisorId не задаётся):
// гос-вертикаль (/gov) ей не управляет, лидер назначается дев-меню (/fdev), а в
// будущем — собственной криминальной механикой. База — интерьер 10 (Four
// Dragons Casino), вход/выход на улице Лас-Вентураса; изоляция по виртуальному
// миру (= id фракции).
class RussianMafiaSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 17; // 1 — АП, 2-4 — полиции, 5-7 — банки, 8 — ФБР, 9-16 — банды/армия

    RussianMafiaSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    FactionService &m_factionService;
};
