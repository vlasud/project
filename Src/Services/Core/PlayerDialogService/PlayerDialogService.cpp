#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Log/LogManager.h"
#include "component.hpp"
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace
{
// Серверный безопасный парс целого из ввода диалога: триммим пробелы по краям,
// отвергаем пустую строку, мусор и переполнение int64 (через std::from_chars —
// без UB, в отличие от stoi/atoi). На любую неудачу возвращаем nullopt.
std::optional<std::int64_t> parseInteger(StringView inputText)
{
    std::string_view text(inputText.data(), inputText.size());

    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    if (text.empty())
        return std::nullopt;

    std::int64_t value = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) // мусор/хвост/overflow
        return std::nullopt;
    return value;
}
} // namespace

void PlayerDialogService::present(IPlayer &player, const Dialog &dialog)
{
    IPlayerDialogData *data = queryExtension<IPlayerDialogData>(player);
    if (!data)
    {
        LogManager::log(Error, "PlayerDialogService: IPlayerDialogData extension is missing");
        return;
    }

    Slot &slot = m_slots[player.getID()];
    slot.serial = slot.serial % 32000 + 1; // id в диапазоне 1..32000, новый на каждый показ
    slot.activeId = slot.serial;

    data->show(player, slot.activeId, dialog.style, dialog.title, dialog.body, dialog.leftButton, dialog.rightButton);
}

void PlayerDialogService::show(IPlayer &player, const Dialog &dialog, Handler handler)
{
    Slot &slot = m_slots[player.getID()];
    slot.inputType = InputType::Text;
    slot.handler = std::move(handler);
    slot.numberHandler = nullptr;
    slot.numberDialog = Dialog{}; // не держим лишнюю копию текста

    present(player, dialog);
}

void PlayerDialogService::showNumberInput(IPlayer &player, const Dialog &dialog, NumberHandler handler)
{
    Slot &slot = m_slots[player.getID()];
    slot.inputType = InputType::Integer;
    slot.handler = nullptr;
    slot.numberHandler = std::move(handler);
    slot.numberDialog = dialog; // копия для повторного показа при неудаче парса

    present(player, dialog);
}

void PlayerDialogService::hide(IPlayer &player)
{
    Slot &slot = m_slots[player.getID()];
    slot.activeId = -1;
    slot.handler = nullptr;
    slot.numberHandler = nullptr;
    slot.numberDialog = Dialog{};

    if (IPlayerDialogData *data = queryExtension<IPlayerDialogData>(player))
    {
        data->hide(player);
    }
}

void PlayerDialogService::handleResponse(IPlayer &player, int dialogId, DialogResponse response, int listItem,
                                         StringView inputText)
{
    Slot &slot = m_slots[player.getID()];

    const bool hasHandler = slot.inputType == InputType::Integer ? static_cast<bool>(slot.numberHandler)
                                                                 : static_cast<bool>(slot.handler);
    if (dialogId != slot.activeId || !hasHandler)
    {
        return; // запоздалый или подделанный ответ
    }

    if (slot.inputType == InputType::Integer)
    {
        // На левой кнопке пробуем распарсить ввод. Удачно — отдаём число; неудачно —
        // повторяем показ ТОГО ЖЕ диалога (событийно: новый показ + ждём следующего
        // ответа, не цикл на сервере) и колбэк НЕ зовём.
        if (response == DialogResponse_Left)
        {
            const std::optional<std::int64_t> value = parseInteger(inputText);
            if (!value)
            {
                // Слот ещё держит numberHandler/numberDialog — пере-арм тем же
                // диалогом. Берём локальную копию диалога: present() меняет слот,
                // а numberDialog мы хотим сохранить для возможных след. повторов.
                Dialog dialog = slot.numberDialog;
                present(player, dialog);
                return;
            }

            // Успех: забираем колбэк до вызова (внутри обычно show() следующего экрана,
            // который займёт слот), гасим текущее ожидание.
            NumberHandler handler = std::move(slot.numberHandler);
            slot.numberHandler = nullptr;
            slot.numberDialog = Dialog{};
            slot.activeId = -1;

            handler(response, *value);
            return;
        }

        // Отмена (правая кнопка / закрытие): не повторяем, отдаём (response, 0).
        NumberHandler handler = std::move(slot.numberHandler);
        slot.numberHandler = nullptr;
        slot.numberDialog = Dialog{};
        slot.activeId = -1;

        handler(response, 0);
        return;
    }

    // Текстовый режим — прежнее поведение.
    // Забираем колбэк до вызова: внутри него обычно show() следующего экрана, который займёт слот.
    Handler handler = std::move(slot.handler);
    slot.handler = nullptr;
    slot.activeId = -1;

    handler(response, listItem, inputText);
}

void PlayerDialogService::resetPlayer(int playerId)
{
    // serial не сбрасываем: он продолжает защищать от запоздалых ответов прошлого подключения.
    Slot &slot = m_slots[playerId];
    slot.activeId = -1;
    slot.handler = nullptr;
    slot.numberHandler = nullptr;
    slot.numberDialog = Dialog{};
}
