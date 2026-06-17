#pragma once

#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/InventoryService/InventoryService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Аптечка — первый предмет поверх базовой системы вещей. В конструкторе
// регистрирует свой тип в InventoryService (как конкретная фракция регистрирует
// себя в FactionService); хранение/персист/выдача — на InventorySystem.
//
// Команда /healme: тратит ОДНУ аптечку и лечит на MEDKIT_HEAL (серверно-
// авторитетно, через PlayerHealthService::heal — зажим по 100/активному кэпу).
//
// Порядок проверок /healme (важен для UX, см. Docs/Inventory.md и UI_Texts.md):
//   1) кэп HP активен (штраф после смерти) — отказ ПЕРВЫМ, не тратим/не лечим
//      (ключевое требование: под штрафом аптечка не работает, иначе обходит
//      DeathPenalty);
//   2) HP уже полное — отказ до проверки наличия (не отнимаем предмет впустую);
//   3) аптечек нет — отказ последним (осмыслен, только когда лечение разрешено).
class MedkitSystem : public BaseSystem
{
  public:
    static constexpr int ITEM_MEDKIT = 1;       // тип предмета «Аптечка» в реестре вещей
    static constexpr float MEDKIT_HEAL = 50.0f; // сколько HP восстанавливает одна аптечка
    static constexpr int MEDKIT_MAX = 3;        // максимум аптечек в стеке

    MedkitSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    void healMe(IPlayer &player);

    InventoryService &m_inventory;
    PlayerHealthService &m_health;
};
