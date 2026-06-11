#pragma once

#include "Macro.h"
#include "Services/Core/CameraService/CameraService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <Server/Components/Objects/objects.hpp>
#include <array>
#include <vector>

// Кинематографическая камера с точками перехода.
//
// /camera — включить свободный полёт (WASD — полёт, Jump/C — вверх/вниз,
// Sprint — ускорение); повторный /camera — меню (во время проигрывания —
// остановка). /cpoint — быстро сохранить текущую позицию и направление взгляда
// как точку. /cplay — проиграть плавную интерполяцию по всем точкам.
// Время сегмента, зацикливание, управление точками — в меню.
//
// Точки хранятся до выхода игрока с сервера: можно выйти из камеры,
// вернуться и проиграть путь снова.
class DebugCameraSystem : public BaseSystem, public PlayerUpdateEventHandler, public PlayerConnectEventHandler
{
  public:
    DebugCameraSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    using CameraPoint = CameraPathPoint; // структура точки общая с CameraService

    struct CameraState
    {
        bool enabled = false;
        Vector3 position{};
        int objectId = -1;        // объект-носитель камеры в полёте; -1 во время проигрывания
        Vector3 returnPosition{}; // куда вернуть тело игрока при выходе
        float speed = 1.0f;       // метров за тик полёта

        std::vector<CameraPoint> points;
        int selectedPoint = -1; // точка, выбранная в меню

        // настройки проигрывания (само проигрывание — в CameraService)
        bool loop = false;
        int segmentTimeMs = 3000; // время одного перелёта между соседними точками
    };

    // --- управление режимом ---
    void enableCamera(IPlayer &player);
    void disableCamera(IPlayer &player);

    // --- полёт ---
    void processFlight(IPlayer &player);
    bool createCameraObject(IPlayer &player); // создать носитель в state.position и привязать камеру
    void releaseCameraObject(CameraState &state);

    // --- точки и проигрывание (само проигрывание делает CameraService) ---
    void savePoint(IPlayer &player);
    Vector3 lookAtPoint(IPlayer &player) const; // точка в 10 м по направлению взгляда
    void startPlayback(IPlayer &player);
    void stopPlayback(IPlayer &player, const Vector3 &restPosition); // вернуть полёт в указанной точке

    // --- экраны диалогов ---
    void showMain(IPlayer &player);
    void showPoints(IPlayer &player);
    void showPointEdit(IPlayer &player);
    void showSegmentTimeInput(IPlayer &player);
    void showSpeedInput(IPlayer &player);
    void showSaveNameInput(IPlayer &player);
    void showLoadList(IPlayer &player, std::vector<std::string> files);

    // --- файлы (диск — в тредпуле, колбэки на главном потоке) ---
    std::string serializePath(const CameraState &state) const;
    void savePathToFileAsync(IPlayer &player, const std::string &name);
    void loadPathFromFileAsync(IPlayer &player, const std::string &name);
    bool loadPathFromContent(CameraState &state, const std::string &content);
    void listPathFilesAsync(IPlayer &player);

    CameraState &stateOf(const IPlayer &player);
    IPlayer *cameraPlayer(int playerId); // игрок, если онлайн и камера включена

    PlayerCommandService &m_commandService;
    PlayerDialogService &m_dialogService;
    PlayerLocationService &m_locationService;
    CameraService &m_cameraService;

    IObjectsComponent *m_objects = nullptr;

    std::array<CameraState, MAX_PLAYERS> m_state;
};
