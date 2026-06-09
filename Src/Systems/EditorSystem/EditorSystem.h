#pragma once

#include "../../Macro.h"
#include "../BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Actors/actors.hpp>
#include <Server/Components/Dialogs/dialogs.hpp>
#include <Server/Components/Objects/objects.hpp>
#include <array>
#include <string>
#include <vector>

// Инструмент маппинга: расстановка объектов и акторов на сцене через свободную камеру и диалоговый интерфейс.
// Команда /editor включает режим и открывает меню.
class EditorSystem : public BaseSystem,
                     public PlayerTextEventHandler,
                     public PlayerUpdateEventHandler,
                     public PlayerDialogEventHandler
{
  public:
    EditorSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    bool onPlayerCommandText(IPlayer &player, StringView message) override;
    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onDialogResponse(IPlayer &player, int dialogId, DialogResponse response, int listItem,
                          StringView inputText) override;

  private:
    enum class EntityType : uint8_t
    {
        Object,
        Actor
    };

    struct EditorEntity
    {
        EntityType type = EntityType::Object;
        int entityId = -1;       // pool id объекта/актора
        int model = 0;           // модель объекта или скин актора
        Vector3 position{};      // позиция
        Vector3 rotation{};      // эйлеровы углы (для актора используется только z как угол)
        std::string animLib;     // анимация актора
        std::string animName;    // анимация актора
    };

    // Текущий экран диалога игрока.
    enum class Screen : uint8_t
    {
        None,
        Main,
        ObjectModelInput,
        ObjectList,
        ObjectEdit,
        ActorSkinInput,
        ActorList,
        ActorEdit,
        RotateInput,
        PosInput,
        DistanceInput,
        ActorAnimLibInput,
        ActorAnimNameInput,
        SaveNameInput,
        LoadList
    };

    struct EditorState
    {
        bool enabled = false;
        Vector3 cameraPosition{};
        int cameraObjectId = -1;     // невидимый объект, к которому привязана камера
        std::vector<EditorEntity> entities;
        int selectedIndex = -1;      // индекс редактируемой сущности
        bool followCamera = false;   // сущность следует за взглядом камеры
        float placeDistance = 6.0f;  // дистанция установки перед камерой
        Screen screen = Screen::None;
        int rotateAxis = 0;          // 0=x,1=y,2=z
        std::string pendingAnimLib;  // буфер при вводе анимации
        std::vector<std::string> loadFiles; // список файлов для экрана загрузки

        bool autoGround = true;      // автоснап к земле при установке
        int groundProbe = -1;        // индекс сущности, для которой ищется земля (FindZ)
        int groundProbeTicks = 0;    // тики ожидания ответа клиента
    };

    // --- управление режимом ---
    void enableEditor(IPlayer &player);
    void disableEditor(IPlayer &player);

    // --- работа с сущностями ---
    Vector3 placementPoint(IPlayer &player) const;
    void createObjectEntity(IPlayer &player, int model);
    void createActorEntity(IPlayer &player, int skin);
    void applyEntityTransform(EditorEntity &entity);
    void deleteEntity(IPlayer &player, int index);
    void requestGroundSnap(IPlayer &player, int index); // запустить поиск земли (FindZ) для сущности
    void processGroundProbe(IPlayer &player);           // обработать ответ клиента в onPlayerUpdate

    // --- диалоги ---
    IPlayerDialogData *getDialog(IPlayer &player);
    void showText(IPlayer &player, DialogStyle style, const std::string &title, const std::string &body,
                  const std::string &btnLeft, const std::string &btnRight);
    void showMain(IPlayer &player);
    void showObjectEdit(IPlayer &player);
    void showActorEdit(IPlayer &player);
    void showObjectList(IPlayer &player);
    void showActorList(IPlayer &player);
    void showLoadList(IPlayer &player);

    void handleMain(IPlayer &player, EditorState &state, DialogResponse response, int listItem);
    void handleObjectEdit(IPlayer &player, EditorState &state, DialogResponse response, int listItem);
    void handleActorEdit(IPlayer &player, EditorState &state, DialogResponse response, int listItem);

    // --- файлы ---
    bool saveToFile(const EditorState &state, const std::string &name, std::string &error);
    bool loadFromFile(IPlayer &player, const std::string &name, std::string &error);
    std::vector<std::string> listMapFiles() const;

    EditorState &stateOf(const IPlayer &player);

    IObjectsComponent *m_objects = nullptr;
    IActorsComponent *m_actors = nullptr;
    IDialogsComponent *m_dialogs = nullptr;

    std::array<EditorState, MAX_PLAYERS> m_state;
};
