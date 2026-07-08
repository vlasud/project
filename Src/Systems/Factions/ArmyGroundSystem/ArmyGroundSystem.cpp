#include "Systems/Factions/ArmyGroundSystem/ArmyGroundSystem.h"

#include "Systems/Factions/PresidentAdministrationSystem/PresidentAdministrationSystem.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const char *const FACTION_NAME = "Армия СВ";
const Colour FACTION_COLOUR{85, 107, 47}; // оливковый
const Vector3 BASE_POS{291.0f, 1817.0f, 18.0f}; // Зона 69, Боун-Каунти
constexpr float BASE_ANGLE = 0.0f;
constexpr int MARKER_MODEL = 1239; // иконка «i»
constexpr int MARKER_TYPE = 1;     // подбор по касанию, всегда виден
} // namespace

ArmyGroundSystem::ArmyGroundSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>()),
      m_pickupService(serviceRegister.getService<PickupService>())
{
    // Госструктура: куратор — Администрация Президента (как полиция/ФБР),
    // лидер назначается через /gov.
    m_factionService.registerFaction(FACTION_ID, FACTION_NAME, PresidentAdministrationSystem::FACTION_ID);
    m_factionService.registerColour(FACTION_ID, FACTION_COLOUR);
    // Единственный военный пед в GTA SA — 287; пул легко расширить позже.
    m_factionService.registerSkins(FACTION_ID, {287});

    // Базы/интерьера нет (registerBase не вызывается). Спавн на территории
    // базы — открытый мир (интерьер 0, мир 0): члены видят друг друга и всех.
    FactionService::Spawn spawn;
    spawn.position = BASE_POS;
    spawn.angle = BASE_ANGLE;
    spawn.interior = 0;
    spawn.virtualWorld = 0;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}

void ArmyGroundSystem::initialize(IComponentList *components)
{
    // Визуальный маркер базы на координатах спавна (стример готов к initialize,
    // как у ElectionSystem). Обработчик подбора уже валиден по
    // дистанции/миру/антиспаму, игрок передаётся ссылкой — null-безопасен.
    m_pickupService.add(MARKER_MODEL, MARKER_TYPE, BASE_POS,
                        [](IPlayer &player) {
                            player.sendClientMessage(FACTION_COLOUR,
                                                     u(fmt::format("Военная база «{}»", FACTION_NAME)));
                        });
}
