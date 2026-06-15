#include "Systems/Factions/RussianMafiaSystem/RussianMafiaSystem.h"

RussianMafiaSystem::RussianMafiaSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    // Без supervisorId — криминальная фракция, гос-вертикалью не курируется.
    m_factionService.registerFaction(FACTION_ID, "Русская мафия");
    m_factionService.registerColour(FACTION_ID, Colour(180, 30, 30)); // тёмно-красный
    m_factionService.registerSkins(FACTION_ID, {111, 112, 120, 121, 122, 123, 124, 125});

    // База: вход/выход на улице (ЛВ), интерьер 5 (Madd Dogg's Mansion);
    // геометрия общая с АП, изоляция по виртуальному миру.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 5;
    // Вход с улицы -> внутрь базы (интерьер 5).
    base.entrances.push_back({{1455.9121f, 751.0028f, 11.0234f}, {1265.5013f, -785.2495f, 1091.9063f}, 269.2356f});
    // Выход из базы -> на улицу.
    base.exits.push_back({{1260.9033f, -785.4575f, 1091.8058f}, {1450.5017f, 751.1109f, 11.0234f}, 88.7914f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {1278.2006f, -816.0803f, 1085.6328f};
    spawn.angle = 179.2038f;
    spawn.interior = 5;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
