#pragma once

#include "Macro.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Actors/actors.hpp>
#include <Server/Components/Objects/objects.hpp>
#include <Server/Components/Pickups/pickups.hpp>
#include <Server/Components/Vehicles/vehicles.hpp>
#include <array>
#include <string>
#include <vector>

// Инструмент маппинга: расстановка объектов и NPC (акторов с анимациями) через
// свободную камеру, горячие клавиши и диалоговый интерфейс.
//
// /editor — вход в редактор и возврат в меню; он же «фиксирует» сущность в
// режимах следования за взглядом и перемещения клавишами.
//
// Камера: WASD/стрелки — полёт, Jump — вверх, C — вниз, Sprint — ускорение.
// Режимы клавиш: стрелки двигают (XY/высота) или вращают выбранную сущность;
// Sprint — крупный шаг, Alt (walk) — точный.
//
// Тело игрока на время редактора спрятано от валидации позиции (bypass): его
// легально таскают FindZ-зонды, и оно вслепую бегает при полёте камеры
// (замораживать нельзя — у замороженного клиента блокируется вращение камеры и
// падает частота синка). На выходе тело возвращается в точку входа в редактор.
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
        Actor,
        Vehicle,
        Pickup
    };

    // Кому сейчас принадлежат стрелки: камере или выбранной сущности.
    enum class KeyMode : uint8_t
    {
        Camera, // стрелки летают камерой (обычный режим)
        MoveXY, // стрелки двигают сущность в горизонтальной плоскости (от взгляда)
        MoveZ,  // вверх/вниз двигают сущность по высоте
        Rotate  // влево/вправо вращают сущность вокруг Z
    };

    struct EditorEntity
    {
        EntityType type = EntityType::Object;
        int entityId = -1;    // pool id объекта/актора/машины
        int model = 0;        // модель объекта/машины или скин актора
        Vector3 position{};   // позиция
        Vector3 rotation{};   // эйлеровы углы (для актора/машины используется только z как угол)
        std::string animLib;  // анимация актора
        std::string animName; // анимация актора
        bool animLoop = true; // true — зациклена; false — стоп-поза (freeze на последнем кадре)
        int colour1 = -1;     // цвета машины (-1 — случайный)
        int colour2 = -1;
        int pickupType = 1;   // клиентский тип поведения пикапа
    };

    struct EditorState
    {
        bool enabled = false;
        Vector3 cameraPosition{};
        int cameraObjectId = -1;  // невидимый объект, к которому привязана камера
        Vector3 returnPosition{}; // куда вернуть тело игрока при выходе из редактора
        float cameraSpeed = 1.0f; // метров за тик полёта камеры
        std::vector<EditorEntity> entities;
        int selectedIndex = -1;     // индекс редактируемой сущности
        bool followCamera = false;  // сущность следует за взглядом камеры
        KeyMode keyMode = KeyMode::Camera;
        float keyStep = 0.25f;      // метров за тик в режимах перемещения клавишами
        float placeDistance = 6.0f; // дистанция установки перед камерой
        bool autoGround = true;     // автоснап к земле при установке
        int groundProbe = -1;       // индекс сущности, для которой ищется земля (FindZ)
        int groundProbeTicks = 0;   // тики ожидания ответа клиента
    };

    // --- управление режимом ---
    void enableEditor(IPlayer &player);
    void disableEditor(IPlayer &player);

    // --- per-tick ---
    void processCameraFlight(IPlayer &player);
    void processKeyMode(IPlayer &player);
    void processGroundProbe(IPlayer &player); // обработать ответ клиента на FindZ
    void cancelGroundProbe(EditorState &state); // вернуть запаркованную сущность и снять зонд

    // --- работа с сущностями ---
    Vector3 placementPoint(IPlayer &player) const;
    void createObjectEntity(IPlayer &player, int model);
    void createActorEntity(IPlayer &player, int skin);
    void createVehicleEntity(IPlayer &player, int model);
    void createPickupEntity(IPlayer &player, int model);
    IVehicle *spawnVehicle(const EditorEntity &entity); // создать машину по данным сущности
    void duplicateEntity(IPlayer &player, int index);
    void applyEntityTransform(EditorEntity &entity);
    void applyEntityAnimation(EditorEntity &entity); // переприменить сохранённую анимацию актора
    bool setActorAnimation(IPlayer &player, EditorEntity &entity, std::string lib, std::string name, bool loop);
    void deleteEntity(IPlayer &player, int index);
    void clearScene(IPlayer &player);
    void requestGroundSnap(IPlayer &player, int index); // запустить поиск земли (FindZ) для сущности
    bool selectedValid(const EditorState &state) const;

    // --- экраны диалогов ---
    void showMain(IPlayer &player);
    void showEntityEdit(IPlayer &player); // диспатч по типу выбранной сущности
    void showObjectEdit(IPlayer &player);
    void showActorEdit(IPlayer &player);
    void showVehicleEdit(IPlayer &player);
    void showPickupEdit(IPlayer &player);
    void showObjectModelInput(IPlayer &player);
    void showActorSkinInput(IPlayer &player);
    void showVehicleModelInput(IPlayer &player);
    void showVehicleColoursInput(IPlayer &player);
    void showPickupModelInput(IPlayer &player);
    void showPickupTypeInput(IPlayer &player);
    void showChangeModelInput(IPlayer &player); // смена модели объекта / скина актора на месте
    void showPositionInput(IPlayer &player);
    void showRotationInput(IPlayer &player); // объект: три угла одной строкой
    void showAngleInput(IPlayer &player);    // актор: один угол
    void showKeyStepInput(IPlayer &player);
    void showDistanceInput(IPlayer &player);
    void showCameraSpeedInput(IPlayer &player);
    void showAnimationMenu(IPlayer &player); // пресеты поз NPC + ручной ввод
    void showAnimLibInput(IPlayer &player);
    void showAnimNameInput(IPlayer &player, std::string animLib);
    void showSaveNameInput(IPlayer &player);
    void showLoadList(IPlayer &player);
    void showLoadModeChoice(IPlayer &player, std::string fileName); // заменить сцену или добавить
    void showClearConfirm(IPlayer &player);
    void showObjectList(IPlayer &player);
    void showActorList(IPlayer &player);
    void showVehicleList(IPlayer &player);
    void showPickupList(IPlayer &player);

    // --- файлы ---
    bool saveToFile(const EditorState &state, const std::string &name, std::string &error);
    bool loadFromFile(IPlayer &player, const std::string &name, std::string &error, std::size_t &loaded);
    std::vector<std::string> listMapFiles() const;

    EditorState &stateOf(const IPlayer &player);
    IPlayer *editorPlayer(int playerId); // игрок, если онлайн и редактор включён

    PlayerDialogService &m_dialogService;
    PlayerCommandService &m_commandService;
    PlayerLocationService &m_locationService; // байпас валидации позиции на время редактора

    IObjectsComponent *m_objects = nullptr;
    IActorsComponent *m_actors = nullptr;
    IVehiclesComponent *m_vehicles = nullptr;
    IPickupsComponent *m_pickups = nullptr;

    std::array<EditorState, MAX_PLAYERS> m_state;
};
