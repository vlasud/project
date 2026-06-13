#pragma once

#include "Services/Core/PickupService/PickupService.h"
#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Армия СВ (Сухопутные войска) — военная база Зона 69
// (Боун-Каунти). Госструктура: курируется Администрацией Президента
// (supervisorId = PresidentAdministrationSystem::FACTION_ID), как полиция/ФБР —
// лидер назначается через /gov. Базы/интерьера нет (registerBase не
// вызывается): спавн прямо на территории базы (мир 0), цвет, пул скинов и
// визуальный пикап-маркер базы.
class ArmyGroundSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 15; // 8 — ФБР, 14 — Aztecas

    ArmyGroundSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    FactionService &m_factionService;
    PickupService &m_pickupService;
};
