#pragma once

#include "Services/FactionService/FactionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Конкретная фракция: Администрация Президента (АП). Образец расширения
// common-системы фракций:
//  * регистрирует себя (id фиксированный — на него завязаны БД и выборы:
//    лидер АП — президент, назначается итогом выборов);
//  * регистрирует свою базу (Madd Dogg's Mansion): пикапы входа/выхода и
//    телепорт делает common-привод FactionSystem;
//  * сюда же лягут её биты маски и геймплейные команды.
class PresidentAdministrationSystem : public BaseSystem
{
  public:
    static constexpr int FACTION_ID = 1;

    PresidentAdministrationSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    FactionService &m_factionService;
};
