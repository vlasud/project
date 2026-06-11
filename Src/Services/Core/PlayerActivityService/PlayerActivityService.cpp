#include "Services/Core/PlayerActivityService/PlayerActivityService.h"

#include "Utils/Encoding/Encoding.h"
#include "core.hpp"
#include <chrono>
#include <fmt/format.h>

namespace
{
// Активный клиент шлёт синк десятки раз в секунду (даже замороженный — ~1 Гц);
// пропажа дольше порога — пауза.
constexpr auto PAUSE_THRESHOLD = std::chrono::milliseconds(3000);
constexpr auto PAUSE_CAP = std::chrono::minutes(30); // дальше точное время не показываем

const Colour LABEL_COLOUR = Colour(220, 220, 220, 255);
constexpr float LABEL_DRAW_DISTANCE = 25.0f;
const Vector3 LABEL_OFFSET{0.0f, 0.0f, 0.35f}; // над головой, выше нейм-тега

// Склонение по числу: one — 1, few — 2..4, many — остальные (с учётом 11..14).
const char *plural(long long n, const char *one, const char *few, const char *many)
{
    const long long mod100 = n % 100;
    if (mod100 >= 11 && mod100 <= 14)
    {
        return many;
    }
    switch (n % 10)
    {
    case 1:
        return one;
    case 2:
    case 3:
    case 4:
        return few;
    default:
        return many;
    }
}
} // namespace

bool PlayerActivityService::isPaused(int playerId) const
{
    return m_slots[playerId].paused;
}

Milliseconds PlayerActivityService::pausedFor(int playerId, TimePoint now) const
{
    const Slot &slot = m_slots[playerId];
    if (!slot.paused)
    {
        return Milliseconds(0);
    }
    return std::chrono::duration_cast<Milliseconds>(now - slot.lastUpdate);
}

void PlayerActivityService::onPaused(Handler handler)
{
    m_pausedHandlers.push_back(std::move(handler));
}

void PlayerActivityService::onResumed(Handler handler)
{
    m_resumedHandlers.push_back(std::move(handler));
}

// ------------------------------------------------------------------ вызовы PlayerActivitySystem

void PlayerActivityService::initialize(ICore *core, ITextLabelsComponent *labels)
{
    m_core = core;
    m_labels = labels;
}

void PlayerActivityService::handleConnect(IPlayer &player, TimePoint now)
{
    Slot &slot = m_slots[player.getID()];
    slot = Slot{};
    slot.lastUpdate = now;
}

void PlayerActivityService::handleUpdate(IPlayer &player, TimePoint now)
{
    Slot &slot = m_slots[player.getID()];
    slot.tracked = true;
    slot.lastUpdate = now;

    if (slot.paused)
    {
        markResumed(player); // синк вернулся — пауза кончилась, лейбл гаснет сразу
    }
}

void PlayerActivityService::sweep(TimePoint now)
{
    if (!m_core)
    {
        return;
    }

    for (IPlayer *player : m_core->getPlayers().entries())
    {
        Slot &slot = m_slots[player->getID()];
        if (!slot.tracked)
        {
            continue; // ещё не играл (коннект, класс-селекшн)
        }

        // Wasted-экран и спектейт синкуются иначе — паузой не считаем.
        const PlayerState state = player->getState();
        if (state == PlayerState_Wasted)
        {
            continue;
        }

        const auto gap = std::chrono::duration_cast<Milliseconds>(now - slot.lastUpdate);
        if (gap < PAUSE_THRESHOLD)
        {
            continue; // активен; возврат из паузы ловит handleUpdate
        }

        if (!slot.paused)
        {
            markPaused(*player, now);
        }
        else
        {
            updateLabel(*player, slot, gap); // тикающее время на лейбле
        }
    }
}

void PlayerActivityService::resetPlayer(int playerId)
{
    hideLabel(m_slots[playerId]);
    m_slots[playerId] = Slot{};
}

// ------------------------------------------------------------------ private

void PlayerActivityService::markPaused(IPlayer &player, TimePoint now)
{
    Slot &slot = m_slots[player.getID()];
    slot.paused = true;
    updateLabel(player, slot, std::chrono::duration_cast<Milliseconds>(now - slot.lastUpdate));

    for (const Handler &handler : m_pausedHandlers)
    {
        handler(player);
    }
}

void PlayerActivityService::markResumed(IPlayer &player)
{
    Slot &slot = m_slots[player.getID()];
    slot.paused = false;
    hideLabel(slot);

    for (const Handler &handler : m_resumedHandlers)
    {
        handler(player);
    }
}

void PlayerActivityService::updateLabel(IPlayer &player, Slot &slot, Milliseconds duration)
{
    if (!m_labels)
    {
        return;
    }

    const std::string text = Encoding::utf8Tocp1251(pauseText(duration));
    if (text == slot.lastText)
    {
        return; // минуты меняются редко — не шлём одинаковые пакеты
    }

    if (slot.labelId < 0)
    {
        ITextLabel *label = m_labels->create(text, LABEL_COLOUR, LABEL_OFFSET, LABEL_DRAW_DISTANCE,
                                             player.getVirtualWorld(), true, player);
        if (!label)
        {
            return; // пул лейблов переполнен — обойдёмся без визуала
        }
        slot.labelId = label->getID();
    }
    else if (ITextLabel *label = m_labels->get(slot.labelId))
    {
        label->setColourAndText(LABEL_COLOUR, text);
    }
    slot.lastText = text;
}

void PlayerActivityService::hideLabel(Slot &slot)
{
    if (slot.labelId >= 0 && m_labels)
    {
        m_labels->release(slot.labelId);
    }
    slot.labelId = -1;
    slot.lastText.clear();
}

std::string PlayerActivityService::pauseText(Milliseconds duration)
{
    const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();

    if (duration > PAUSE_CAP)
    {
        return "На паузе >30 минут";
    }
    if (seconds >= 60)
    {
        const long long minutes = seconds / 60;
        return fmt::format("На паузе {} {}", minutes, plural(minutes, "минуту", "минуты", "минут"));
    }
    return fmt::format("На паузе {} {}", seconds, plural(seconds, "секунду", "секунды", "секунд"));
}
