#include "Systems/Factions/TriadSystem/TriadSystem.h"

TriadSystem::TriadSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    // Без supervisorId — криминальная фракция, гос-вертикалью не курируется.
    m_factionService.registerFaction(FACTION_ID, "Триада");
    m_factionService.registerColour(FACTION_ID, Colour(200, 30, 30)); // красный
    m_factionService.registerSkins(FACTION_ID, {49, 55, 117, 118, 120, 121, 122, 123, 186, 193, 208, 224, 263, 294});

    // База: вход/выход на улице (СФ), интерьер 1; изоляция по виртуальному миру.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 1;
    // Вход с улицы -> внутрь базы (интерьер 1).
    base.entrances.push_back({{-2192.6472f, 647.4241f, 49.4375f}, {-2158.7942f, 640.8340f, 1052.3817f}, 178.9830f});
    // Выход из базы -> на улицу.
    base.exits.push_back({{-2158.6802f, 643.1420f, 1052.3750f}, {-2192.6394f, 644.2915f, 49.4375f}, 174.7120f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {-2167.9275f, 644.2442f, 1052.3750f};
    spawn.angle = 234.7569f;
    spawn.interior = 1;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
