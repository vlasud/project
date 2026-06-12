#include "Systems/Factions/PoliceLasVenturasSystem/PoliceLasVenturasSystem.h"

#include "Systems/Factions/PoliceSkins.h"
#include "Systems/Factions/PresidentAdministrationSystem/PresidentAdministrationSystem.h"

PoliceLasVenturasSystem::PoliceLasVenturasSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    m_factionService.registerFaction(FACTION_ID, "Полиция Лас-Вентураса", PresidentAdministrationSystem::FACTION_ID);
    m_factionService.registerColour(FACTION_ID, Colour(0, 102, 255)); // синий, как у остальных полиций
    m_factionService.registerSkins(FACTION_ID, POLICE_SKINS); // пул общий на всю полицию

    // База — участок LVPD (интерьер 3): входы с улицы и с гаража,
    // выходы на улицу и в гараж.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 3;
    base.entrances.push_back({{2287.1411f, 2432.3923f, 11.0003f}, {238.6470f, 142.9408f, 1003.0234f}, 360.0f});
    base.entrances.push_back({{2282.2659f, 2423.6440f, 3.9450f}, {288.5125f, 172.0496f, 1007.1794f}, 0.0f});
    base.exits.push_back({{238.6312f, 138.7888f, 1003.1146f}, {2287.0925f, 2428.0012f, 10.8203f}, 180.0f});
    base.exits.push_back({{288.7545f, 167.3622f, 1007.1719f}, {2282.2095f, 2427.3772f, 3.2734f}, 0.0f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников — внутри участка (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {281.3177f, 182.1192f, 1007.1719f};
    spawn.angle = 106.0f;
    spawn.interior = 3;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
