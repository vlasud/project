#pragma once

#include "Services/Core/PickupService/PickupService.h"
#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Grove Street Families — банда (Гантон, Лос-Сантос).
// Криминальная, кураторством Администрации Президента НЕ управляется
// (supervisorId не задаётся), как Триада/Rifa: /gov ей не управляет, лидер —
// через /fdev. База с интерьером 3: вход с улицы (только членам), спавн внутри
// базы; изоляция по виртуальному миру (= id фракции). Плюс цвет, пул скинов и
// визуальный пикап-маркер территории на турфе.
class GroveStreetSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 11; // 9 — Триада, 10 — Rifa

    GroveStreetSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    FactionService &m_factionService;
    PickupService &m_pickupService;
};
