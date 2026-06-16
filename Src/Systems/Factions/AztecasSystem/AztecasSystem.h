#pragma once

#include "Services/Core/PickupService/PickupService.h"
#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Varrios Los Aztecas — банда (Эль-Корона, ЛС).
// Криминальная, кураторством Администрации Президента НЕ управляется
// (supervisorId не задаётся), как Триада/Rifa: /gov ей не управляет, лидер —
// через /fdev. База с интерьером 5: вход с улицы (только членам), спавн внутри
// базы; изоляция по виртуальному миру (= id фракции). Плюс цвет, пул скинов и
// визуальный пикап-маркер территории на турфе.
class AztecasSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 14; // 9 — Триада, 10 — Rifa, 11-13 — Grove/Ballas/Vagos

    AztecasSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    FactionService &m_factionService;
    PickupService &m_pickupService;
};
