#pragma once

#include "../../Macro.h"
#include "../IService.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "types.hpp"
#include <array>
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

    void show(IPlayer &player, const Dialog &dialog, Handler handler);
    void hide(IPlayer &player);

  private:
    // Вызываются только PlayerDialogSystem.
    void handleResponse(IPlayer &player, int dialogId, DialogResponse response, int listItem, StringView inputText);
    void resetPlayer(int playerId);

    struct Slot
    {
        int activeId = -1; // id текущего показанного диалога, -1 — диалога нет
        int serial = 0;    // циклический счётчик для генерации id
        Handler handler;
    };

    std::array<Slot, MAX_PLAYERS> m_slots;
};
