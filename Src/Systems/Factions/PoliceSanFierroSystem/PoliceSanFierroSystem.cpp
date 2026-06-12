#include "Systems/Factions/PoliceSanFierroSystem/PoliceSanFierroSystem.h"

#include "Systems/Factions/PoliceSkins.h"
#include "Systems/Factions/PresidentAdministrationSystem/PresidentAdministrationSystem.h"

PoliceSanFierroSystem::PoliceSanFierroSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    m_factionService.registerFaction(FACTION_ID, "Полиция Сан-Фиерро", PresidentAdministrationSystem::FACTION_ID);
    m_factionService.registerColour(FACTION_ID, Colour(0, 102, 255)); // синий, как у остальных полиций
    m_factionService.registerSkins(FACTION_ID, POLICE_SKINS);         // пул общий на всю полицию

    // База — участок SFPD (интерьер 10): входы с улицы и из гаража,
    // выходы на улицу и в гараж.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 10;
    base.entrances.push_back({{-1605.5656f, 710.2726f, 13.8672f}, {246.4020f, 110.7531f, 1003.2188f}, 0.0f});
    base.entrances.push_back({{-1594.2118f, 716.1957f, -4.9063f}, {246.9688f, 123.8954f, 1003.2188f}, 180.0f});
    base.exits.push_back({{246.3704f, 107.2999f, 1003.2188f}, {-1605.5602f, 713.3346f, 13.5311f}, 0.0f});
    base.exits.push_back({{247.0465f, 126.7359f, 1003.2188f}, {-1590.9684f, 716.1979f, -5.2422f}, 270.0f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников — внутри участка (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {274.2394f, 125.3913f, 1004.6172f};
    spawn.angle = 90.0f;
    spawn.interior = 10;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
