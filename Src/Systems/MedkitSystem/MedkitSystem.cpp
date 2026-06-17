#include "Systems/MedkitSystem/MedkitSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <cmath>
#include <fmt/format.h>

namespace
{
// Успех /healme — INFO_COLOUR (голубой), как системно-информационные подтверждения
// (репорт, /savepos): лечение — положительное событие-инфо.
const Colour INFO_COLOUR{120, 220, 255};
// Отказы /healme — ERROR_COLOUR (красный), в ряд с «Игрок не найден»: красным
// игрок сразу читает «не сработало».
const Colour ERROR_COLOUR{255, 90, 90};

// Допуск на «полное здоровье»: тот же масштаб, что EPS рассинхрона в
// PlayerHealthService — float HP не всегда ровно 100.
constexpr float FULL_HEALTH_EPS = 1.0f;

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

MedkitSystem::MedkitSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_inventory(serviceRegister.getService<InventoryService>()),
      m_health(serviceRegister.getService<PlayerHealthService>())
{
    // Регистрация типа предмета в базовой системе вещей (как фракция себя в
    // FactionService). Имя — utf-8 (отображение/дев-меню конвертируют сами).
    m_inventory.registerItem(ITEM_MEDKIT, "Аптечка", MEDKIT_MAX);

    serviceRegister.getService<PlayerCommandService>().add(
        "healme", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { healMe(player); }, {},
        "использовать аптечку и восстановить здоровье", PlayerCommandService::HelpCategory::Misc);
}

void MedkitSystem::healMe(IPlayer &player)
{
    const int playerId = player.getID();

    // 1) Штраф после смерти (общий кэп HP активен) — отказ ПЕРВЫМ: под штрафом
    // лечиться нельзя ничем, и сообщить об этом надо раньше «нет аптечек», иначе
    // игрок побежит искать аптечки, не поняв причину. Не тратим, не лечим.
    if (m_health.hasMaxHealth(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы ещё не оправились после смерти — лечиться нельзя"));
        return;
    }

    // 2) Полное здоровье — отказ до проверки наличия: не отнимаем предмет впустую и
    // честно объясняем почему.
    if (m_health.getHealth(playerId) >= PlayerHealthService::MAX_HEALTH - FULL_HEALTH_EPS)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас полное здоровье — аптечка не нужна"));
        return;
    }

    // 3) Наличие аптечек — отказ последним: осмыслен, только когда лечение в
    // принципе разрешено и нужно.
    if (m_inventory.count(playerId, ITEM_MEDKIT) == 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас нет аптечек"));
        return;
    }

    // Лечение: серверно-авторитетно (heal зажимает по 100/активному кэпу; кэпа тут
    // уже нет — проверен выше). Аптечку снимаем ПОСЛЕ успешного лечения, ровно одну.
    const float hpBefore = m_health.getHealth(playerId);
    m_health.heal(player, MEDKIT_HEAL);
    const float restored = m_health.getHealth(playerId) - hpBefore;
    // Лечение могло не примениться (heal — no-op для мёртвого/умирающего: окно
    // серверной смерти HP=0/dying до подтверждения клиентом). Тогда аптечку НЕ
    // тратим — расход только при реально восстановленном HP.
    if (restored <= 0.0f)
        return;
    m_inventory.remove(playerId, ITEM_MEDKIT, 1);

    const int left = m_inventory.count(playerId, ITEM_MEDKIT);
    // {hp} — ФАКТИЧЕСКИ восстановленное HP (с 60 до 100 → +40, не +50).
    player.sendClientMessage(
        INFO_COLOUR,
        u(fmt::format("Вы использовали аптечку: +{} HP. Осталось аптечек: {}", static_cast<int>(std::lround(restored)),
                      left)));
}
