#pragma once

#include "Macro.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Actors/actors.hpp>
#include <Server/Components/Objects/objects.hpp>
#include <array>
#include <string>
#include <vector>

// Инструмент маппинга: расстановка объектов и акторов на сцене через свободную камеру и диалоговый интерфейс.
// Команда /editor включает режим и открывает меню.
class EditorSystem : public BaseSystem, public PlayerUpdateEventHandler, public PlayerConnectEventHandler
{
  public:
    EditorSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    enum class EntityType : uint8_t
    {
        Object,
        Actor
    };

    struct EditorEntity
    {
        EntityType type = EntityType::Object;
        int entityId = -1;    // pool id объекта/актора
        int model = 0;        // модель объекта или скин актора
        Vector3 position{};   // позиция
        Vector3 rotation{};   // эйлеровы углы (для актора используется только z как угол)
        std::string animLib;  // анимация актора
        std::string animName; // анимация актора
    };

    struct EditorState
    {
        bool enabled = false;
        Vector3 cameraPosition{};
        int cameraObjectId = -1; // невидимый объект, к которому привязана камера
        std::vector<EditorEntity> entities;
        int selectedIndex = -1;     // индекс редактируемой сущности
        bool followCamera = false;  // сущность следует за взглядом камеры
        float placeDistance = 6.0f; // дистанция установки перед камерой
        bool autoGround = true;     // автоснап к земле при установке
        int groundProbe = -1;       // индекс сущности, для которой ищется земля (FindZ)
        int groundProbeTicks = 0;   // тики ожидания ответа клиента
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

    // --- экраны диалогов ---
    void showMain(IPlayer &player);
    void showEntityEdit(IPlayer &player); // диспатч в Object/Actor по типу выбранной сущности
    void showObjectEdit(IPlayer &player);
    void showActorEdit(IPlayer &player);
    void showObjectModelInput(IPlayer &player);
    void showActorSkinInput(IPlayer &player);
    void showRotateInput(IPlayer &player, int axis);
    void showPosInput(IPlayer &player);
    void showDistanceInput(IPlayer &player);
    void showAnimLibInput(IPlayer &player);
    void showAnimNameInput(IPlayer &player, std::string animLib);
    void showSaveNameInput(IPlayer &player);
    void showObjectList(IPlayer &player);
    void showActorList(IPlayer &player);
    void showLoadList(IPlayer &player);

    // --- файлы ---
    bool saveToFile(const EditorState &state, const std::string &name, std::string &error);
    bool loadFromFile(IPlayer &player, const std::string &name, std::string &error);
    std::vector<std::string> listMapFiles() const;

    EditorState &stateOf(const IPlayer &player);
    IPlayer *editorPlayer(int playerId); // игрок, если онлайн и редактор включён

    PlayerDialogService &m_dialogService;
    PlayerCommandService &m_commandService;
    PlayerLocationService &m_locationService; // байпас валидации позиции на время редактора

    IObjectsComponent *m_objects = nullptr;
    IActorsComponent *m_actors = nullptr;

    std::array<EditorState, MAX_PLAYERS> m_state;
};
