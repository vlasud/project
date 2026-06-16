#include "Systems/Factions/GroveStreetSystem/GroveStreetSystem.h"

#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const char *const FACTION_NAME = "Grove Street";
const Colour FACTION_COLOUR{0, 153, 0}; // зелёный
const Vector3 TURF_POS{2495.0f, -1688.0f, 13.5f}; // турф Гантон
constexpr int MARKER_MODEL = 1239; // иконка «i»
constexpr int MARKER_TYPE = 1;     // подбор по касанию, всегда виден

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

GroveStreetSystem::GroveStreetSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>()),
      m_pickupService(serviceRegister.getService<PickupService>())
{
    // Без supervisorId — криминальная банда, гос-вертикалью не курируется.
    m_factionService.registerFaction(FACTION_ID, FACTION_NAME);
    m_factionService.registerColour(FACTION_ID, FACTION_COLOUR);
    m_factionService.registerSkins(FACTION_ID, {105, 106, 107});

    // База банды: вход с улицы, интерьер 3; изоляция по виртуальному
    // миру (= id фракции), как у прочих организаций с базой.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 3;
    // Вход с улицы -> внутрь базы (интерьер 3).
    base.entrances.push_back({{2495.3560f, -1691.1332f, 14.7656f}, {2496.3589f, -1695.5648f, 1014.7422f}, 177.9277f});
    // Выход из базы -> на улицу.
    base.exits.push_back({{2495.9290f, -1692.0836f, 1014.7422f}, {2495.2234f, -1687.9604f, 13.5161f}, 1.3725f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн членов — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {2494.8315f, -1707.3843f, 1018.3368f};
    spawn.angle = 269.3986f;
    spawn.interior = 3;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}

void GroveStreetSystem::initialize(IComponentList *components)
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
