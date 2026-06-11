#pragma once

#include "Macro.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <string>
#include <vector>

// Редактор textdraw: команда /td открывает диалоговое меню, через которое можно
// создать textdraw, настроить любое его свойство, подвигать клавишами по экрану,
// выбрать textdraw кликом мыши, сохранить набор в файл и загрузить обратно.
// Редактор работает на per-player текстдравах — правки видит только сам игрок.
class TextDrawEditorSystem : public BaseSystem, public PlayerUpdateEventHandler, public PlayerConnectEventHandler
{
  public:
    TextDrawEditorSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    struct Item
    {
        int textDrawId = -1;
        bool selectable = false; // желаемое значение; на время выбора кликом форсируется true
    };

    struct Session
    {
        bool enabled = false;
        std::vector<Item> items;
        int selected = -1;      // индекс редактируемого textdraw
        bool moveMode = false;  // перемещение клавишами в onPlayerUpdate
        float moveStep = 1.0f;  // пикселей за тик
        bool picking = false;   // активен режим «выбрать кликом»
    };

    // --- управление режимом ---
    void enableEditor(IPlayer &player);
    void disableEditor(IPlayer &player);
    void startMoveMode(IPlayer &player);
    void stopMoveMode(IPlayer &player);
    void startPicking(IPlayer &player);
    void stopPicking(IPlayer &player); // снять выбор кликом и вернуть selectable как было

    // --- работа с textdraw ---
    int createItem(IPlayer &player, Vector2 position, StringView text, const TextDrawParams &params);
    void deleteItem(IPlayer &player, int index);
    void deleteAllItems(IPlayer &player);
    IPlayerTextDraw *itemTextDraw(IPlayer &player, int index);
    IPlayerTextDraw *selectedTextDraw(IPlayer &player); // выбранный или nullptr (сбрасывает мёртвый выбор)
    int indexOfTextDraw(const Session &session, int textDrawId) const;

    // --- экраны диалогов ---
    void showMain(IPlayer &player);
    void showCreateTextInput(IPlayer &player);
    void showCreateModelInput(IPlayer &player);
    void showList(IPlayer &player);
    void showEdit(IPlayer &player);
    void showTextInput(IPlayer &player);
    void showPositionInput(IPlayer &player);
    void showMoveStepInput(IPlayer &player);
    void showStylePicker(IPlayer &player);
    void showLetterSizeInput(IPlayer &player);
    void showTextSizeInput(IPlayer &player);
    void showAlignmentPicker(IPlayer &player);
    enum class ColourTarget : uint8_t
    {
        Letter,
        Box,
        Background
    };
    void showColourInput(IPlayer &player, ColourTarget target);
    void showShadowInput(IPlayer &player);
    void showOutlineInput(IPlayer &player);
    void showPreviewModelInput(IPlayer &player);
    void showPreviewRotationInput(IPlayer &player);
    void showPreviewZoomInput(IPlayer &player);
    void showSaveNameInput(IPlayer &player);
    void showLoadList(IPlayer &player);

    // --- файлы ---
    bool saveToFile(IPlayer &player, const std::string &name, std::string &error);
    bool loadFromFile(IPlayer &player, const std::string &name, std::string &error);
    std::vector<std::string> listTextDrawFiles() const;

    Session &sessionOf(const IPlayer &player);
    IPlayer *editorPlayer(int playerId); // игрок, если онлайн и редактор включён

    PlayerDialogService &m_dialogService;
    PlayerCommandService &m_commandService;
    TextDrawService &m_textDrawService;

    std::array<Session, MAX_PLAYERS> m_sessions;
};
