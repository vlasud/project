#pragma once

#include "Macro.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class CameraSystem;
class PlayerConnectionVersionService;
struct ICore;

struct CameraPathPoint
{
    Vector3 position{}; // позиция камеры
    Vector3 lookAt{};   // точка, куда камера смотрит
};

struct CameraPath
{
    std::vector<CameraPathPoint> points; // для проигрывания нужно минимум две
    Milliseconds segmentTime{3000};      // время перелёта между соседними точками
    bool loop = false;                   // после последней — плавно к первой и по кругу
};

// Кинематографическая камера: проигрывание путей с плавной интерполяцией
// позиции и взгляда (катсцены входа, обзор дома при покупке, финал работы).
//
// Пути либо регистрируются кодом, либо лениво грузятся из camerapaths/<имя>.txt
// (формат файлов тулзы /camera; диск — на воркере тредпула):
//
//   m_camera.playFromFile(player, "intro", [](IPlayer &p) { /* катсцена кончилась */ });
//   m_camera.play(player, path, onFinish);
//   m_camera.stop(player); // вернуть камеру за спину
//
// Контракты:
//  * onFinish зовётся РОВНО ОДИН РАЗ по завершении незацикленного пути; при
//    stop(), дисконнекте и перезапуске play() — не зовётся;
//  * зацикленный путь играет до stop();
//  * камеру игрока на время проигрывания никто другой трогать не должен
//    (привязка к объекту перебивает интерполяцию).
//
// Сегменты движутся пер-плеерными таймаутами TimerService: в тиках сервис не
// работает вообще, на дисконнекте таймер гаснет сам.
class CameraService final : public IService
{
    friend CameraSystem;

  public:
    using FinishHandler = std::function<void(IPlayer &)>;
    using LoadHandler = std::function<void(bool ok)>;

    static constexpr std::size_t MAX_POINTS = 64;

    // Разбор файла пути (формат camerapaths). Публичный — переиспользует тулза.
    static bool parsePath(const std::string &content, CameraPath &out);

    // --- реестр путей ---
    void registerPath(const std::string &name, CameraPath path);
    bool hasPath(const std::string &name) const;
    // Асинхронно загрузить camerapaths/<имя>.txt в реестр (имя валидируется от
    // обхода каталогов). onLoaded(ok) — на главном потоке.
    void loadPathFromFile(const std::string &name, LoadHandler onLoaded = nullptr);

    // --- проигрывание ---
    // false — путь невалиден (меньше двух точек). Повторный play заменяет текущий.
    bool play(IPlayer &player, const CameraPath &path, FinishHandler onFinish = nullptr);
    // Путь из реестра; если ещё не загружен — подгрузит из файла и запустит
    // (с защитой от смены игрока в слоте за время чтения).
    bool playFromFile(IPlayer &player, const std::string &name, FinishHandler onFinish = nullptr);
    void stop(IPlayer &player, bool restoreCamera = true);
    bool isPlaying(int playerId) const;
    int segmentIndex(int playerId) const; // активный сегмент (для инструментов)

  private:
    struct Slot
    {
        bool playing = false;
        CameraPath path; // копия: реестр может меняться во время проигрывания
        int segment = 0; // активный сегмент: points[i] -> points[(i+1) % n]
        TimerService::Handle timer{};
        FinishHandler onFinish;
    };

    // Вызываются CameraSystem.
    void initialize(ICore *core, TimerService *timers, PlayerConnectionVersionService *versions);
    void resetPlayer(int playerId);

    void advance(IPlayer &player); // сегмент закончился — следующий или финиш
    void scheduleAdvance(IPlayer &player, Slot &slot);
    void interpolateSegment(IPlayer &player, const CameraPathPoint &from, const CameraPathPoint &to,
                            Milliseconds time);
    static CameraPath sanitizePath(CameraPath path);
    static bool validFileName(const std::string &name);

    ICore *m_core = nullptr;
    TimerService *m_timers = nullptr;
    PlayerConnectionVersionService *m_versions = nullptr;

    std::unordered_map<std::string, CameraPath> m_paths;
    std::array<Slot, MAX_PLAYERS> m_slots;
};
