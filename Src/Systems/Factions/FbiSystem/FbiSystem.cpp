#include "Systems/Factions/FbiSystem/FbiSystem.h"

#include "Systems/Factions/PresidentAdministrationSystem/PresidentAdministrationSystem.h"

FbiSystem::FbiSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>())
{
    m_factionService.registerFaction(FACTION_ID, "ФБР", PresidentAdministrationSystem::FACTION_ID);
    m_factionService.registerColour(FACTION_ID, Colour(40, 55, 95)); // тёмно-синий «федеральный»
    // Пул скинов ФБР: агент FBI (286) + деловые/официальные образы.
    m_factionService.registerSkins(FACTION_ID, {286, 166, 165, 164, 163, 150, 141, 17});

    // Своя точка входа/выхода на улице (СФ); интерьер и внутренние позиции —
    // общие с участком SFPD (интерьер 10), изоляция по виртуальному миру.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 10;
    // Вход с улицы (точка ФБР) -> внутрь участка SFPD.
    base.entrances.push_back({{-2456.1497f, 503.9155f, 30.0781f}, {246.4020f, 110.7531f, 1003.2188f}, 0.0f});
    // Выход из участка -> на улицу (точка ФБР).
    base.exits.push_back({{246.3704f, 107.2999f, 1003.2188f}, {-2452.9482f, 504.0042f, 30.0812f}, 269.9001f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн сотрудников — внутри участка (как у полиции СФ), мир базы = id фракции.
    FactionService::Spawn spawn;
    spawn.position = {274.2394f, 125.3913f, 1004.6172f};
    spawn.angle = 90.0f;
    spawn.interior = 10;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}
