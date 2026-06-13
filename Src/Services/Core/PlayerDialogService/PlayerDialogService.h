#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>

class PlayerDialogSystem;

// Диалог — значение, собираемое на месте показа. Поведение (реакция на ответ)
// передаётся отдельным колбэком в show() и живёт ровно до одного ответа игрока.
struct Dialog
{
    DialogStyle style = DialogStyle_MSGBOX;
    std::string title;
    std::string body;
    std::string leftButton;
    std::string rightButton;
};

// Per-player роутер диалогов: хранит ожидающий колбэк и id показанного диалога.
// Id генерируется заново на каждый показ, поэтому запоздалые ответы на прошлые
// диалоги (в т.ч. от прошлого подключения в том же слоте) отбрасываются.
class PlayerDialogService final : public IService
{
    friend PlayerDialogSystem;

  public:
    using Handler = std::function<void(DialogResponse response, int listItem, StringView inputText)>;
    // Колбэк для числового INPUT-диалога: получает уже распарсенное целое.
    // На отмене (response != Left) value == 0 — вызывающий смотрит на response.
    using NumberHandler = std::function<void(DialogResponse response, std::int64_t value)>;

    void show(IPlayer &player, const Dialog &dialog, Handler handler);

    // INPUT-диалог с числовым вводом: сервис сам парсит ввод как целое.
    // Успех — колбэк получает число; нераспарсенный ввод (мусор/пусто/overflow)
    // на левой кнопке — сервис повторяет показ ТОГО ЖЕ диалога (событийно, не цикл
    // на сервере), колбэк не зовётся. Диапазон значения проверяет вызывающий.
    void showNumberInput(IPlayer &player, const Dialog &dialog, NumberHandler handler);

    void hide(IPlayer &player);

  private:
    // Вызываются только PlayerDialogSystem.
    void handleResponse(IPlayer &player, int dialogId, DialogResponse response, int listItem, StringView inputText);
    void resetPlayer(int playerId);

    // Тип вводимого значения для INPUT-диалога.
    enum class InputType
    {
        Text,    // текущее поведение: отдаём сырой StringView (через Handler)
        Integer, // парсим в int64 (через NumberHandler), на неудаче — повтор показа
    };

    // Общая точка показа: генерит id/serial, армит слот и шлёт диалог клиенту.
    void present(IPlayer &player, const Dialog &dialog);

    struct Slot
    {
        int activeId = -1;                   // id текущего показанного диалога, -1 — диалога нет
        int serial = 0;                      // циклический счётчик для генерации id
        InputType inputType = InputType::Text;
        Handler handler;                     // активен при inputType == Text
        NumberHandler numberHandler;         // активен при inputType == Integer
        Dialog numberDialog;                 // копия диалога для повторного показа при неудаче парса
    };

    std::array<Slot, MAX_PLAYERS> m_slots;
};
