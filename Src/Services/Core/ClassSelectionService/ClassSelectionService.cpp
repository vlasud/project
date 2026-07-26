#include "Services/Core/ClassSelectionService/ClassSelectionService.h"

#include "Log/LogManager.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>

namespace
{
const char *BLOCKED_MESSAGE = "Эти кнопки не работают. Вы не должны были их увидеть";

// Легальный RequestSpawn приходит после клиентского wasted-экрана, но его
// длительность плавает (зависит от анимации смерти). Запрос раньше порога —
// подозрение на пропуск экрана смерти читом: фиксируем, но НЕ отклоняем —
// отказ роняет клиент в класс-селекшн с кнопками, а выгода чита — лишь
// несколько секунд.
constexpr auto EARLY_RESPAWN_SUSPECT = std::chrono::milliseconds(1500);
} // namespace

void ClassSelectionService::setEntryHandler(EntryHandler handler)
{
    m_entryHandler = std::move(handler);
}

void ClassSelectionService::initialize(AntiCheatService *antiCheat)
{
    m_antiCheat = antiCheat;
}

bool ClassSelectionService::handleRequestClass(IPlayer &player, TimePoint now)
{
    Slot &slot = m_slots[player.getID()];

    if (slot.inSelection)
    {
        // Повторные запросы до спавна — нажатия дефолтных стрелок ◄ ►.
        player.sendClientMessage(Colour::White(), Encoding::utf8Tocp1251(BLOCKED_MESSAGE));
        return false;
    }

    // Вход в класс-селекшн легален в двух случаях: подключение (ещё ни разу не
    // спавнился) и F4 + смерть (смерть зафиксирована сервером). RequestClass от
    // живого игрока — подделка ради телепорта+хила через respawn.
    if (slot.everSpawned && !slot.deathPending)
    {
        if (m_antiCheat)
        {
            m_antiCheat->record(player.getID(), AntiCheatService::ViolationType::SpawnHack,
                                "request class while alive (respawn abuse attempt)", now);
        }
        player.sendClientMessage(Colour::White(), Encoding::utf8Tocp1251(BLOCKED_MESSAGE));
        return false;
    }

    slot.inSelection = true;
    if (m_entryHandler)
    {
        m_entryHandler(player);
    }
    else
    {
        LogManager::log(Error, "ClassSelectionService: no entry handler set, player is stuck in class selection");
    }
    // Легальному входу отвечаем true (Selectable=true): ответ false клиент
    // трактует как «класс-селекшн не завершён» и после КАЖДОЙ смерти
    // возвращается в него вместо авто-респауна. Полистать классы игрок всё
    // равно не успевает: обработчик уже заспавнил его, а повторные запросы
    // (стрелки) отклоняются выше.
    return true;
}

bool ClassSelectionService::handleRequestSpawn(IPlayer &player, TimePoint now)
{
    Slot &slot = m_slots[player.getID()];

    if (slot.deathPending)
    {
        if (now - slot.diedAt < EARLY_RESPAWN_SUSPECT && m_antiCheat)
        {
            m_antiCheat->record(player.getID(), AntiCheatService::ViolationType::SpawnHack,
                                "request spawn too early after death (death screen skip)", now);
        }
        return true; // авто-респаун после смерти — в точку спавна, без класс-селекшна
    }

    // Кнопка Spawn в класс-селекшне — сообщение; запрос от живого — подделка.
    player.sendClientMessage(Colour::White(), Encoding::utf8Tocp1251(BLOCKED_MESSAGE));
    if (slot.everSpawned && !slot.inSelection && m_antiCheat)
    {
        m_antiCheat->record(player.getID(), AntiCheatService::ViolationType::SpawnHack,
                            "request spawn while alive", now);
    }
    return false;
}

void ClassSelectionService::handleDeath(IPlayer &player, TimePoint now)
{
    Slot &slot = m_slots[player.getID()];
    slot.deathPending = true;
    slot.diedAt = now;
}

void ClassSelectionService::handleSpawn(IPlayer &player)
{
    Slot &slot = m_slots[player.getID()];
    slot.inSelection = false;
    slot.everSpawned = true;
    slot.deathPending = false;
}

void ClassSelectionService::resetPlayer(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    m_slots[playerId] = Slot{};
}
