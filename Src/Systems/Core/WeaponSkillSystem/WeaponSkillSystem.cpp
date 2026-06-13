#include "Systems/Core/WeaponSkillSystem/WeaponSkillSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};

// Имена скиллов в порядке PlayerWeaponSkill (0..10) — только для дев-вывода.
constexpr const char *SKILL_NAMES[WeaponSkillService::NUM_SKILLS] = {
    "Pistol", "Silenced", "Deagle", "Shotgun", "SawnOff", "SPAS12", "Uzi", "MP5", "AK47", "M4", "Sniper"};

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

WeaponSkillSystem::WeaponSkillSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_weaponSkillService(serviceRegister.getService<WeaponSkillService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);

    // Дев-тулза: одна команда -> диалог-меню (серверная правда + максимум/сброс).
    serviceRegister.getService<PlayerCommandService>().add(
        "wskill", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showSkillMenu(player); });
}

void WeaponSkillSystem::showSkillMenu(IPlayer &player)
{
    const int playerId = player.getID();
    std::string body = "Оружие\tУровень\n";
    for (int index = 0; index < WeaponSkillService::NUM_SKILLS; ++index)
    {
        body += fmt::format("{}\t{}\n", SKILL_NAMES[index],
                            m_weaponSkillService.getLevel(playerId, static_cast<PlayerWeaponSkill>(index)));
    }

    Dialog dialog;
    dialog.style = DialogStyle_TABLIST_HEADERS;
    dialog.title = u("Weapon skills (серверная правда)");
    dialog.body = u(body);
    dialog.leftButton = u("В максимум");
    dialog.rightButton = u("Сбросить");

    // playerId, не ссылка: игрок мог выйти, пока диалог открыт (висячая ссылка).
    m_dialogService.show(player, dialog,
                         [this, playerId](DialogResponse response, int, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response == DialogResponse_Left)
                             {
                                 m_weaponSkillService.maxOut(*player);
                                 player->sendClientMessage(INFO_COLOUR, u("Все скиллы оружия -> 999"));
                             }
                             else if (response == DialogResponse_Right)
                             {
                                 m_weaponSkillService.reset(*player);
                                 player->sendClientMessage(INFO_COLOUR, u("Все скиллы оружия -> 0"));
                             }
                         });
}

void WeaponSkillSystem::onPlayerConnect(IPlayer &player)
{
    // Чистое стартовое состояние: слот мог остаться занятым прошлым игроком.
    m_weaponSkillService.resetPlayer(player.getID());
}

void WeaponSkillSystem::onPlayerSpawn(IPlayer &player)
{
    // Переприменяем сохранённый набор — клиент гарантированно получает ровно то,
    // что лежит в источнике правды, при каждой жизни.
    m_weaponSkillService.reapply(player);
}

void WeaponSkillSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_weaponSkillService.resetPlayer(player.getID());
}
