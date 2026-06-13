#include "Systems/Factions/RifaSystem/RifaSystem.h"

RifaSystem::RifaSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    // Без supervisorId — криминальная фракция, гос-вертикалью не курируется.
    m_factionService.registerFaction(FACTION_ID, "Rifa");
    m_factionService.registerColour(FACTION_ID, Colour(110, 70, 210)); // фиолетово-синий
    m_factionService.registerSkins(FACTION_ID, {173, 174, 175, 226, 184, 242, 273, 44, 56});

    // База: вход/выход на улице (СФ), интерьер 17; изоляция по виртуальному миру.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 17;
    // Вход с улицы -> внутрь базы (интерьер 17).
    base.entrances.push_back({{-2454.5952f, -135.8841f, 26.1911f}, {493.4720f, -21.0992f, 1000.6797f}, 359.8820f});
    // Выход из базы -> на улицу.
    base.exits.push_back({{493.3535f, -24.6206f, 1000.6797f}, {-2457.7007f, -135.8744f, 26.0050f}, 92.3422f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {502.6735f, -4.2630f, 1000.6797f};
    spawn.angle = 131.0421f;
    spawn.interior = 17;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
