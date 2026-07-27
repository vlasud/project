#pragma once

#include "Macro.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <Server/Components/Vehicles/vehicles.hpp>
#include <array>
#include <string>

// Сервис стейта и спец-экшена — единственный источник истины для PlayerState и
// PlayerSpecialAction, плюс валидация переходов.
//
// Стейт выводится ядром из того, КАКОЙ sync шлёт клиент (footSync → OnFoot,
// driverSync → Driver...), то есть клиент им управляет. Легальные переходы идут
// через промежуточные фазы (OnFoot → EnterVehicleDriver → Driver с анимацией
// посадки); чит шлёт driverSync сразу — мгновенная посадка/угон. Сервис валидирует
// каждый переход: фаза входа обязательна, вход не быстрее физического, дистанция
// до машины в момент посадки правдоподобна. Серверные перемещения (putInVehicle,
// setSpectating) санкционируют свои переходы.
//
// Спец-экшен клиент-авторитетен в footSync. Принимается всё, кроме:
//  * джетпака без серверной выдачи (классический чит) — снимается + нарушение;
//  * побега из принудительного экшена (enforced, например Cuffed) — клиент,
//    вышедший из наручников, переустанавливается + нарушение.
//
// КОНТРАКТ: серверные смены стейта/экшена — ТОЛЬКО через этот сервис
// (putInVehicle/removeFromVehicle/setSpectating/setSpecialAction). Прямые вызовы
// мимо сервиса валидатор посчитает читом.
class PlayerStateService final : public IService
{
  public:
    // Вызывается PlayerStateSystem при создании.
    void bind(PlayerLocationService &location);

    // --- источник истины ---
    PlayerState getState(int playerId) const;
    PlayerSpecialAction getSpecialAction(int playerId) const;

    // --- серверные операции (санкционируют переходы) ---
    void putInVehicle(IPlayer &player, IVehicle &vehicle, int seat);
    void removeFromVehicle(IPlayer &player);
    void setSpectating(IPlayer &player, bool spectating);

    // enforced: клиент не вправе снять экшен сам (Cuffed) — побег фиксируется и
    // переустанавливается. Не-enforced (выпивка, телефон) клиент завершает сам.
    void setSpecialAction(IPlayer &player, PlayerSpecialAction action, bool enforced = false);
    void clearSpecialAction(IPlayer &player);

    struct StateOutcome
    {
        bool stateHack = false;
        std::string detail;
    };
    struct ActionOutcome
    {
        bool actionHack = false;
        // Игрок не выполнил серверную высадку и продолжает активно играть за рулём.
        // Система должна применить РЕАЛЬНЫЙ рычаг — респавн машины: сам RPC высадки
        // клиент вправе игнорировать, а вот машину у него из-под сиденья забирает
        // сервер, и ядро после этого перестаёт принимать его driver-sync.
        bool enforceEject = false;
        int ejectVehicleId = -1;
        std::string detail;
    };

    // --- вызывается PlayerStateSystem / VehicleSystem ---
    StateOutcome onStateChange(IPlayer &player, PlayerState newState, PlayerState oldState, TimePoint now);
    ActionOutcome verifyAction(IPlayer &player, TimePoint now);
    // Игрок начал вход в машину (onPlayerEnterVehicle — нажатие Enter у двери).
    // Ядро на практике НЕ эмитит стейты EnterVehicle*, легальная посадка выглядит
    // как OnFoot -> Driver напрямую — фаза входа подтверждается этим событием.
    void onEnterVehicle(IPlayer &player, int vehicleId, TimePoint now);
    void onSpawn(IPlayer &player);
    void reset(int playerId);

  private:
    struct State
    {
        PlayerState state = PlayerState_None;                  // принятый стейт
        PlayerSpecialAction action = SpecialAction_None;       // принятый экшен
        PlayerSpecialAction serverAction = SpecialAction_None; // последняя серверная выдача
        bool actionEnforced = false;
        TimePoint actionChange; // последняя серверная смена экшена (грейс синхронизации)

        TimePoint enterStart;    // начало фазы входа в ТС (onPlayerEnterVehicle)
        int enterVehicleId = -1; // в какую машину начат вход

        bool pendingPut = false; // санкция putInVehicle
        TimePoint putAt;
        // Ожидание высадки. Отсчёт в ПРИНЯТЫХ апдейтах, а не по часам: на паузе и
        // при лаге апдейты не идут, мир клиента заморожен, и высадка физически не
        // может продвинуться — по стенным часам это выглядело бы как игнор.
        bool pendingEject = false;
        int ejectVehicleId = -1;
        std::uint16_t ejectUpdates = 0;
        bool pendingSpectate = false; // санкция setSpectating
        bool spectateTarget = false;  // в какую сторону
        TimePoint spectateAt;
    };

    bool consumePutSanction(State &st, TimePoint now);
    bool consumeSpectateSanction(State &st, bool target, TimePoint now);

    PlayerLocationService *m_location = nullptr;

    std::array<State, MAX_PLAYERS> m_state;
};
