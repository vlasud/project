#include "Systems/Factions/ItalianMafiaSystem/ItalianMafiaSystem.h"

ItalianMafiaSystem::ItalianMafiaSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    // Без supervisorId — криминальная фракция, гос-вертикалью не курируется.
    m_factionService.registerFaction(FACTION_ID, "Итальянская мафия");
    m_factionService.registerColour(FACTION_ID, Colour(0, 120, 50)); // тёмно-зелёный
    m_factionService.registerSkins(FACTION_ID, {111, 112, 113, 114, 115, 116});

    // База: вход/выход на улице (ЛВ), интерьер 1 (Caligula's Casino); изоляция
    // по виртуальному миру.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 1;
    // Вход с улицы -> внутрь базы (интерьер 1).
    base.entrances.push_back({{2196.9661f, 1677.1689f, 12.3672f}, {2234.3838f, 1709.9376f, 1011.0534f}, 182.6736f});
    // Выход из базы -> на улицу.
    base.exits.push_back({{2233.8975f, 1714.6837f, 1012.3828f}, {2192.6577f, 1677.0051f, 12.3672f}, 90.0305f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {2194.9126f, 1577.4683f, 1008.3667f};
    spawn.angle = 286.0746f;
    spawn.interior = 1;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
