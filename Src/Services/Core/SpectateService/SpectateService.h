#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <Server/Components/Vehicles/vehicles.hpp>
#include <array>
#include <functional>

class SpectateSystem;
class PlayerStateService;
class PlayerLocationService;
class VehicleService;
struct ICore;

// Сервис спектейта (почва для админки: /spec, наблюдение за репортами).
//
//   m_spectate.spectatePlayer(admin, target, [](IPlayer &a, StopReason r) { ... });
//   m_spectate.spectateVehicle(admin, vehicle);
//   m_spectate.spectatePlayer(admin, other);  // смена цели без выхода
//   m_spectate.stop(admin);                   // вернуть на место, где начинал
//
// Сервис сам решает классические проблемы спектейта:
//  * цель в другом интерьере/мире — наблюдателю синхронизируются interior/vw,
//    его невидимое тело подтягивается к цели (иначе цель не стримится);
//  * цель умерла/респавнулась — спектейт слетает у клиента, сервис
//    переприменяет его на спавне цели;
//  * цель вышла/машина уничтожена — стоп с причиной TargetLost;
//  * выход из спектейта — это респаун: сервис возвращает наблюдателя в точку,
//    интерьер и мир, где тот начинал смотреть. Экипировку GTA на респауне
//    теряет — перевыдача за бизнесом (админке об этом помнить).
//
// Спектейт авторизации (PlayerAuthSystem) сервис не трогает — тот свой
// setSpectating зовёт напрямую и в слотах сервиса не числится.
class SpectateService final : public IService
{
    friend SpectateSystem;

  public:
    enum class StopReason : std::uint8_t
    {
        Manual,     // stop() из кода
        TargetLost, // цель вышла / машина уничтожена
    };

    using StopHandler = std::function<void(IPlayer &spectator, StopReason)>;

    // false — цель совпадает с наблюдателем. Повторный вызов меняет цель,
    // точка возврата остаётся от первого входа в спектейт.
    bool spectatePlayer(IPlayer &spectator, IPlayer &target, StopHandler onStop = nullptr);
    bool spectateVehicle(IPlayer &spectator, IVehicle &target, StopHandler onStop = nullptr);
    void stop(IPlayer &spectator); // StopReason::Manual

    bool isSpectating(int playerId) const;        // спектейтит через ЭТОТ сервис
    int spectateTargetPlayer(int playerId) const; // id цели-игрока или -1

  private:
    struct Slot
    {
        bool active = false;
        bool vehicleTarget = false;
        int targetId = -1;
        // куда вернуть наблюдателя при выходе
        Vector3 returnPosition{};
        unsigned returnInterior = 0;
        int returnWorld = 0;
        bool pendingReturn = false; // ждём респауна после setSpectating(false)
        StopHandler onStop;
    };

    // Вызываются SpectateSystem.
    void initialize(ICore *core, PlayerStateService *state, PlayerLocationService *location,
                    VehicleService *vehicleService);
    void sweep();                       // троттленный догон цели (интерьер/мир/тело)
    void handleSpawn(IPlayer &player);  // возврат наблюдателя + переприменение зрителям цели
    void resetPlayer(int playerId);     // дисконнект: и как наблюдатель, и как цель

    void beginSession(IPlayer &spectator);
    void applySpectate(IPlayer &spectator, Slot &slot); // камера + интерьер/мир/тело
    void stopInternal(IPlayer &spectator, StopReason reason);

    ICore *m_core = nullptr;
    PlayerStateService *m_state = nullptr;
    PlayerLocationService *m_location = nullptr;
    VehicleService *m_vehicleService = nullptr;

    std::array<Slot, MAX_PLAYERS> m_slots;
};
