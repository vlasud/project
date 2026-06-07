#pragma once

#include "../../Macro.h"
#include "../IService.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "types.hpp"
#include <array>
#include <functional>
#include <string>

class PlayerDialogSystem;

struct Dialog
{
    std::string title;
    std::string body;
    std::string leftButton;
    std::string rightButton;
    DialogStyle style = DialogStyle_MSGBOX;

    std::function<void(int, int, StringView)> leftAction;
    std::function<void(int, int, StringView)> rightAction;
};

class PlayerDialogService final : public IService
{
    friend PlayerDialogSystem;

  public:
    int buildDialog(Dialog &&);
    void showDialog(IPlayer &player, int dialogId);

  private:
    bool validateDialog(int playerId, int dialogId);
    Dialog &getPlayerDialog(int playerId);

    IPlayerDialogData *m_dialogExtension = nullptr;

    std::array<int, MAX_PLAYERS> m_playersCurrentDialogId{};
    std::vector<Dialog> m_dialogs;
};
