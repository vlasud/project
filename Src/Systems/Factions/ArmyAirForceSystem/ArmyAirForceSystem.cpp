#include "Systems/Factions/ArmyAirForceSystem/ArmyAirForceSystem.h"

#include "Systems/Factions/PresidentAdministrationSystem/PresidentAdministrationSystem.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const char *const FACTION_NAME = "Армия ВВС";
const Colour FACTION_COLOUR{40, 100, 180}; // синий
const Vector3 BASE_POS{400.0f, 2480.0f, 16.5f}; // аэродром Вердант-Медоуз
constexpr float BASE_ANGLE = 0.0f;
constexpr int MARKER_MODEL = 1239; // иконка «i»
constexpr int MARKER_TYPE = 1;     // подбор по касанию, всегда виден

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

ArmyAirForceSystem::ArmyAirForceSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>()),
      m_pickupService(serviceRegister.getService<PickupService>())
{
    // Госструктура: куратор — Администрация Президента (как полиция/ФБР),
    // лидер назначается через /gov.
    m_factionService.registerFaction(FACTION_ID, FACTION_NAME, PresidentAdministrationSystem::FACTION_ID);
    m_factionService.registerColour(FACTION_ID, FACTION_COLOUR);
    // Отдельного скина ВВС в каноне SA нет — единственный военный пед 287
    // (тот же, что у СВ); пул легко расширить позже.
    m_factionService.registerSkins(FACTION_ID, {287});

    // Базы/интерьера нет (registerBase не вызывается). Спавн на территории
    // аэродрома — открытый мир (интерьер 0, мир 0): члены видят друг друга и всех.
    FactionService::Spawn spawn;
    spawn.position = BASE_POS;
    spawn.angle = BASE_ANGLE;
    spawn.interior = 0;
    spawn.virtualWorld = 0;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}

void ArmyAirForceSystem::initialize(IComponentList *components)
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
