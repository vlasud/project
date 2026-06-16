#pragma once

#include "Services/Core/PickupService/PickupService.h"
#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Los Santos Vagos — банда (Восточный Лос-Сантос).
// Криминальная, кураторством Администрации Президента НЕ управляется
// (supervisorId не задаётся), как Триада/Rifa: /gov ей не управляет, лидер —
// через /fdev. База с интерьером 2: вход с улицы (только членам), спавн внутри
// базы; изоляция по виртуальному миру (= id фракции). Плюс цвет, пул скинов и
// визуальный пикап-маркер территории на турфе.
class VagosSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 13; // 9 — Триада, 10 — Rifa, 11 — Grove Street, 12 — Ballas

    VagosSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    FactionService &m_factionService;
    PickupService &m_pickupService;
};
