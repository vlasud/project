#pragma once

#include "Macro.h"
#include "Services/Core/GangZoneService/GangZoneService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <string>
#include <vector>

// Редактор ганг-зон: /gzone открывает меню. Зона создаётся стандартного
// размера в позиции игрока, дальше двигается и растягивается клавишами
// (стрелки; Sprint — крупный шаг, Alt — точный), смотреть удобно на радаре
// или карте. Перекрытия режутся сервисом автоматически и видны вживую при
// перемещении/растяжении. Цвет, приоритет, точный ввод прямоугольника — в
// меню; наборы сохраняются/загружаются в файлы gangzones/*.txt.
class GangZoneEditorSystem : public BaseSystem, public PlayerUpdateEventHandler, public PlayerConnectEventHandler
{
  public:
    GangZoneEditorSystem(ICore &core, const ServiceRegister &serviceRegister);

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    // Что делают стрелки с выбранной зоной.
    enum class KeyMode : uint8_t
    {
        None,
        Move,  // вверх/вниз — север/юг, влево/вправо — запад/восток
        Resize // вверх/вниз — высота зоны, влево/вправо — ширина (вокруг центра)
    };

    struct Session
    {
        int selectedId = -1; // редактируемая зона
        TimerService::Handle flashTimer{};

        KeyMode keyMode = KeyMode::None;
        float keyStep = 2.0f;    // метров за тик
        GangZonePos editRect{};  // текущий прямоугольник в режиме клавиш
        bool editDirty = false;  // есть несённые в сервис изменения
        int rebuildCooldown = 0; // тиков до следующего применения (троттлинг пересборки)
    };

    // --- действия ---
    void createZoneAt(IPlayer &player);
    void startKeyMode(IPlayer &player, KeyMode mode);
    void stopKeyMode(IPlayer &player); // применяет несённые изменения
    void highlightZone(IPlayer &player, int zoneId);
    void stretchNearestCorner(IPlayer &player, int zoneId);
    void moveZoneCenterHere(IPlayer &player, int zoneId);

    // --- экраны диалогов ---
    void showMain(IPlayer &player);
    void showZoneList(IPlayer &player);
    void showZoneEdit(IPlayer &player);
    void showColourInput(IPlayer &player);
    void showRectInput(IPlayer &player);
    void showKeyStepInput(IPlayer &player);
    void showSaveNameInput(IPlayer &player);
    void showLoadList(IPlayer &player, std::vector<std::string> files);
    void showClearConfirm(IPlayer &player);

    // --- файлы (диск — в тредпуле, колбэки на главном потоке) ---
    std::string serializeZones() const;
    void saveToFileAsync(IPlayer &player, const std::string &name);
    void loadFromFileAsync(IPlayer &player, const std::string &name);
    std::size_t loadFromContent(const std::string &content); // заменяет текущий набор зон
    void listZoneFilesAsync(IPlayer &player);

    Session &sessionOf(const IPlayer &player);
    IPlayer *onlinePlayer(int playerId);

    PlayerDialogService &m_dialogService;
    PlayerCommandService &m_commandService;
    PlayerLocationService &m_locationService;
    TimerService &m_timerService;
    GangZoneService &m_gangZoneService;

    std::array<Session, MAX_PLAYERS> m_sessions;
};
