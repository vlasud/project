#pragma once

#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Банк — самостоятельная финансовая организация (НЕ путать
// с BankSystem — это банковские СЧЕТА игроков, экономика). Гос-вертикалью не
// курируется (supervisorId не задаётся): /gov ей не управляет, лидер
// назначается дев-меню (/fdev). База — интерьер 3, вход/выход на улице
// Сан-Фиерро; изоляция по виртуальному миру (= id фракции).
class BankFactionSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 5; // 1 — АП, 2-4 — полиции, 6-7 — банки городов, 8 — ФБР, 9-18 — банды/армия/мафии

    BankFactionSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    FactionService &m_factionService;
};
