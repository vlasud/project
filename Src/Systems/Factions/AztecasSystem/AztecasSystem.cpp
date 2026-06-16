#include "Systems/Factions/AztecasSystem/AztecasSystem.h"

#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const char *const FACTION_NAME = "Aztecas";
const Colour FACTION_COLOUR{0, 191, 196}; // бирюзовый
const Vector3 TURF_POS{1812.0f, -2025.0f, 13.5f}; // турф Эль-Корона
constexpr int MARKER_MODEL = 1239; // иконка «i»
constexpr int MARKER_TYPE = 1;     // подбор по касанию, всегда виден

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

AztecasSystem::AztecasSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>()),
      m_pickupService(serviceRegister.getService<PickupService>())
{
    // Без supervisorId — криминальная банда, гос-вертикалью не курируется.
    m_factionService.registerFaction(FACTION_ID, FACTION_NAME);
    m_factionService.registerColour(FACTION_ID, FACTION_COLOUR);
    m_factionService.registerSkins(FACTION_ID, {114, 115, 116});

    // База банды: вход с улицы, интерьер 5; изоляция по виртуальному
    // миру (= id фракции), как у прочих организаций с базой.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 5;
    // Вход с улицы -> внутрь базы (интерьер 5).
    base.entrances.push_back({{1673.6543f, -2122.4409f, 14.1460f}, {318.4979f, 1117.2830f, 1083.8828f}, 355.7517f});
    // Выход из базы -> на улицу.
    base.exits.push_back({{318.6117f, 1114.4801f, 1083.8828f}, {1676.5319f, -2120.0833f, 13.5469f}, 312.3691f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн членов — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {308.9395f, 1122.1415f, 1083.8828f};
    spawn.angle = 272.1954f;
    spawn.interior = 5;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}

void AztecasSystem::initialize(IComponentList *components)
{
    // Визуальный маркер территории на турфе (стример готов к initialize, как у
    // ElectionSystem). Обработчик подбора уже валиден по дистанции/миру/антиспаму,
    // игрок передаётся ссылкой — null-безопасен.
    m_pickupService.add(MARKER_MODEL, MARKER_TYPE, TURF_POS,
                        [](IPlayer &player) {
                            player.sendClientMessage(FACTION_COLOUR,
                                                     u(fmt::format("Территория банды «{}»", FACTION_NAME)));
                        });
}
