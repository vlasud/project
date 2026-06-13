#pragma once

#include "Services/Core/PickupService/PickupService.h"
#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Армия ВВС (Военно-Воздушные Силы) — аэродром
// Вердант-Медоуз. Госструктура: курируется Администрацией Президента
// (supervisorId = PresidentAdministrationSystem::FACTION_ID), как полиция/ФБР —
// лидер назначается через /gov. Базы/интерьера нет (registerBase не
// вызывается): спавн прямо на территории аэродрома (мир 0), цвет, пул скинов и
// визуальный пикап-маркер базы.
class ArmyAirForceSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 16; // 8 — ФБР, 14 — Aztecas, 15 — Армия СВ

    ArmyAirForceSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    FactionService &m_factionService;
    PickupService &m_pickupService;
};
