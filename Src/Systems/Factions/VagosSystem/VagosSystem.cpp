#include "Systems/Factions/VagosSystem/VagosSystem.h"

#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const char *const FACTION_NAME = "Vagos";
const Colour FACTION_COLOUR{255, 204, 0}; // жёлтый
const Vector3 TURF_POS{2710.0f, -1370.0f, 13.5f}; // турф Восточный ЛС
constexpr int MARKER_MODEL = 1239; // иконка «i»
constexpr int MARKER_TYPE = 1;     // подбор по касанию, всегда виден

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

VagosSystem::VagosSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_factionService(serviceRegister.getService<FactionService>()),
      m_pickupService(serviceRegister.getService<PickupService>())
{
    // Без supervisorId — криминальная банда, гос-вертикалью не курируется.
    m_factionService.registerFaction(FACTION_ID, FACTION_NAME);
    m_factionService.registerColour(FACTION_ID, FACTION_COLOUR);
    m_factionService.registerSkins(FACTION_ID, {108, 109, 110});

    // База банды: вход с улицы, интерьер 2; изоляция по виртуальному
    // миру (= id фракции), как у прочих организаций с базой.
    FactionService::Base base;
    base.pickupModel = 1318;
    base.interior = 2;
    // Вход с улицы -> внутрь базы (интерьер 2).
    base.entrances.push_back({{2756.3308f, -1182.8099f, 69.4035f}, {222.9930f, 1240.0991f, 1082.1406f}, 89.2970f});
    // Выход из базы -> на улицу.
    base.exits.push_back({{226.7888f, 1239.9597f, 1082.1406f}, {2756.4854f, -1179.6387f, 69.3991f}, 1.2913f});
    m_factionService.registerBase(FACTION_ID, base);

    // Спавн членов — внутри базы (мир базы = id фракции).
    FactionService::Spawn spawn;
    spawn.position = {223.6305f, 1252.4435f, 1082.1406f};
    spawn.angle = 115.9073f;
    spawn.interior = 2;
    spawn.virtualWorld = FACTION_ID;
    m_factionService.registerSpawn(FACTION_ID, spawn);
}

void VagosSystem::initialize(IComponentList *components)
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
