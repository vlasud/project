#include "Systems/Factions/RussianMafiaSystem/RussianMafiaSystem.h"

RussianMafiaSystem::RussianMafiaSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    // Без supervisorId — криминальная фракция, гос-вертикалью не курируется.
    m_factionService.registerFaction(FACTION_ID, "Русская мафия");
    m_factionService.registerColour(FACTION_ID, Colour(180, 30, 30)); // тёмно-красный
    m_factionService.registerSkins(FACTION_ID, {111, 112, 120, 121, 122, 123, 124, 125});

    // База: вход/выход на улице (ЛВ), интерьер 10 (Four Dragons Casino);
    // изоляция по виртуальному миру.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 10;
    // Вход с улицы -> внутрь базы (интерьер 10).
    base.entrances.push_back({{2019.3136f, 1007.6828f, 10.8203f}, {2015.5223f, 1017.6082f, 996.8750f}, 89.6816f});
    // Выход из базы -> на улицу.
    base.exits.push_back({{2019.0714f, 1017.8354f, 996.8750f}, {2024.3163f, 1007.6201f, 10.8203f}, 269.1189f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {1933.3059f, 1063.1820f, 994.4688f};
    spawn.angle = 244.1331f;
    spawn.interior = 10;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
