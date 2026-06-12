#include "Systems/Factions/PoliceLosSantosSystem/PoliceLosSantosSystem.h"

#include "Systems/Factions/PoliceSkins.h"
#include "Systems/Factions/PresidentAdministrationSystem/PresidentAdministrationSystem.h"

PoliceLosSantosSystem::PoliceLosSantosSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    m_factionService.registerFaction(FACTION_ID, "Полиция Лос-Сантоса", PresidentAdministrationSystem::FACTION_ID);
    m_factionService.registerColour(FACTION_ID, Colour(0, 102, 255)); // синий
    m_factionService.registerSkins(FACTION_ID, POLICE_SKINS); // пул общий на всю полицию

    // База — участок LSPD (интерьер 6): два входа (главный и задний двор)
    // и два выхода.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 6;
    base.entrances.push_back({{1554.8367f, -1675.6823f, 16.3769f}, {246.7304f, 65.8676f, 1003.6406f}, 0.0f});
    base.entrances.push_back({{1568.6693f, -1690.3417f, 6.4821f}, {245.1752f, 66.3015f, 1003.6406f}, 270.0f});
    base.exits.push_back({{246.8285f, 62.7487f, 1003.8668f}, {1551.0846f, -1675.6398f, 15.6935f}, 90.0f});
    base.exits.push_back({{242.4284f, 66.3887f, 1003.8668f}, {1568.6466f, -1690.2809f, 5.8906f}, 0.0f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников — внутри участка (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {218.7751f, 78.6873f, 1005.0391f};
    spawn.angle = 270.0f;
    spawn.interior = 6;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
