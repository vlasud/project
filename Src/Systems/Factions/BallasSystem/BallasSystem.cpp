#include "Systems/Factions/BallasSystem/BallasSystem.h"

#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const char *const FACTION_NAME = "Ballas";
const Colour FACTION_COLOUR{170, 60, 190}; // фиолетовый
const Vector3 TURF_POS{2178.0f, -1791.0f, 13.4f}; // турф Айдлвуд
constexpr int MARKER_MODEL = 1239; // иконка «i»
constexpr int MARKER_TYPE = 1;     // подбор по касанию, всегда виден

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

BallasSystem::BallasSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>()),
      m_pickupService(serviceRegister.getService<PickupService>())
{
    // Без supervisorId — криминальная банда, гос-вертикалью не курируется.
    m_factionService.registerFaction(FACTION_ID, FACTION_NAME);
    m_factionService.registerColour(FACTION_ID, FACTION_COLOUR);
    m_factionService.registerSkins(FACTION_ID, {102, 103, 104});

    // База банды: вход с улицы, интерьер 2; изоляция по виртуальному
    // миру (= id фракции), как у прочих организаций с базой.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 2;
    // Вход с улицы -> внутрь базы (интерьер 2).
    base.entrances.push_back({{1939.0682f, -1114.4838f, 27.4523f}, {2465.1985f, -1698.3708f, 1013.5078f}, 88.8212f});
    // Выход из базы -> на улицу.
    base.exits.push_back({{2468.8420f, -1698.2937f, 1013.5078f}, {1939.1842f, -1117.1135f, 26.4455f}, 180.7641f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн членов — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {2450.2490f, -1692.5789f, 1013.5078f};
    spawn.angle = 154.7030f;
    spawn.interior = 2;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}

void BallasSystem::initialize(IComponentList *components)
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
