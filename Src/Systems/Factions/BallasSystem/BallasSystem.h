#pragma once

#include "Services/Core/PickupService/PickupService.h"
#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Ballas — уличная банда (Айдлвуд, Лос-Сантос).
// Криминальная, кураторством Администрации Президента НЕ управляется
// (supervisorId не задаётся), как Триада/Rifa: /gov ей не управляет, лидер —
// через /fdev. В отличие от организаций с базой здесь НЕТ интерьера и дверей
// (registerBase не вызывается): спавн прямо на турфе (улица, мир 0), цвет, пул
// скинов и визуальный пикап-маркер территории.
class BallasSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 12; // 9 — Триада, 10 — Rifa, 11 — Grove Street

    BallasSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    FactionService &m_factionService;
    PickupService &m_pickupService;
};
