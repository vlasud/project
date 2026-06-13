#include "Systems/Factions/BallasSystem/BallasSystem.h"

#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const char *const FACTION_NAME = "Ballas";
const Colour FACTION_COLOUR{170, 60, 190}; // фиолетовый
const Vector3 TURF_POS{2178.0f, -1791.0f, 13.4f}; // турф Айдлвуд
constexpr float TURF_ANGLE = 0.0f;
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

    // Уличная банда: базы/интерьера нет (registerBase не вызывается). Спавн на
    // турфе — публичная улица (интерьер 0, мир 0): члены видят друг друга и всех.
    FactionService::Spawn spawn;
    spawn.position = TURF_POS;
    spawn.angle = TURF_ANGLE;
    spawn.interior = 0;
    spawn.virtualWorld = 0;
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
