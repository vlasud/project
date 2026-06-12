#include "Systems/Factions/PresidentAdministrationSystem/PresidentAdministrationSystem.h"

PresidentAdministrationSystem::PresidentAdministrationSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    m_factionService.registerFaction(FACTION_ID, "Администрация Президента");
    m_factionService.registerColour(FACTION_ID, Colour(30, 144, 255)); // правительственный синий
    // Скины чиновников: деловые костюмы и офисные образы.
    m_factionService.registerSkins(
        FACTION_ID, {9, 17, 57, 59, 76, 91, 141, 147, 148, 163, 164, 165, 166, 185, 186, 187, 227, 228, 295});

    // База АП: вход у Mulholland, интерьер — Madd Dogg's Mansion (5).
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 5;
    base.entrances.push_back({{1123.5710f, -2036.9669f, 70.1744f}, {1265.5013f, -785.2495f, 1091.9063f}, 269.2356f});
    base.exits.push_back({{1260.9033f, -785.4575f, 1091.8058f}, {1130.9860f, -2037.1515f, 69.0078f}, 269.8469f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников АП — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {1278.2006f, -816.0803f, 1085.6328f};
    spawn.angle = 179.2038f;
    spawn.interior = 5;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
