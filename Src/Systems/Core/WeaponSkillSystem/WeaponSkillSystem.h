#pragma once

#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/WeaponSkillService/WeaponSkillService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Проводник WeaponSkillService: переприменяет сохранённые уровни на спавне и
// сбрасывает состояние при подключении/выходе. Плюс дев-команда /wskill —
// диалог с серверной правдой по навыкам и кнопками максимум/сброс.
//
// Переприменение на спавне делает уровни детерминированными независимо от того,
// что серверно случилось с навыками между жизнями (источник правды — наш
// массив, а не текущее состояние клиента). На тик система ничего не делает —
// горячих путей нет, только редкие событийные вызовы.
class WeaponSkillSystem : public BaseSystem, public PlayerConnectEventHandler, public PlayerSpawnEventHandler
{
  public:
    WeaponSkillSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerSpawn(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    void showSkillMenu(IPlayer &player); // дев-меню /wskill

    WeaponSkillService &m_weaponSkillService;
    PlayerDialogService &m_dialogService;
};
