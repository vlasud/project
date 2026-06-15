#include "Systems/Factions/BankFactionSystem/BankFactionSystem.h"

BankFactionSystem::BankFactionSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    // Без supervisorId — самостоятельная организация, гос-вертикалью не курируется.
    m_factionService.registerFaction(FACTION_ID, "Банк");
    m_factionService.registerColour(FACTION_ID, Colour(60, 160, 90)); // зелёный (деньги)
    m_factionService.registerSkins(FACTION_ID, {147, 148, 163, 164, 165, 166});

    // База: вход/выход на улице (СФ), интерьер 3; изоляция по виртуальному миру.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 3;
    // Вход с улицы -> внутрь базы (интерьер 3).
    base.entrances.push_back({{-2766.5515f, 375.5889f, 6.3347f}, {385.7435f, 173.9601f, 1008.3828f}, 90.2254f});
    // Выход из базы -> на улицу.
    base.exits.push_back({{390.7698f, 173.7943f, 1008.3828f}, {-2761.9106f, 375.7773f, 5.4151f}, 270.4663f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {366.9437f, 207.5594f, 1008.3828f};
    spawn.angle = 177.9595f;
    spawn.interior = 3;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
