#include "PlayerStateService.h"

#include "glm/geometric.hpp"
#include <chrono>
#include <fmt/format.h>

namespace
{
// Минимальная длительность фазы входа в ТС: легальная анимация посадки занимает
// от ~0.6 с даже на байке; порог сознательно ниже — ловим только мгновенное.
constexpr std::chrono::milliseconds MIN_ENTER{200};

// Максимум от принятой позиции игрока до машины в момент посадки. Щедро из-за
// габаритов (дверь AT-400 далеко от центра) и лага — ловим вход «через карту».
constexpr float ENTER_MAX_DIST = 30.0f;

// Сколько живёт санкция серверной операции до её потребления переходом.
constexpr std::chrono::milliseconds SANCTION_WINDOW{3000};

// Грейс синхронизации экшена после серверной установки/переустановки.
constexpr std::chrono::milliseconds ACTION_GRACE{1500};

TimePoint now()
{
    return std::chrono::steady_clock::now();
}
} // namespace

void PlayerStateService::bind(PlayerLocationService &location)
{
    m_location = &location;
}

PlayerState PlayerStateService::getState(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return PlayerState_None;
    return m_state[playerId].state;
}

PlayerSpecialAction PlayerStateService::getSpecialAction(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return SpecialAction_None;
    return m_state[playerId].action;
}

void PlayerStateService::putInVehicle(IPlayer &player, IVehicle &vehicle, int seat)
{
    State &st = m_state[player.getID()];
    st.pendingPut = true;
    st.putAt = now();
    vehicle.putPlayer(player, seat);
}

void PlayerStateService::removeFromVehicle(IPlayer &player)
{
    // Выход из ТС в OnFoot легален из любого состояния — санкция не нужна.
    player.removeFromVehicle(false);
}

void PlayerStateService::setSpectating(IPlayer &player, bool spectating)
{
    State &st = m_state[player.getID()];
    st.pendingSpectate = true;
    st.spectateTarget = spectating;
    st.spectateAt = now();
    player.setSpectating(spectating);
}

void PlayerStateService::setSpecialAction(IPlayer &player, PlayerSpecialAction action, bool enforced)
{
    State &st = m_state[player.getID()];
    st.serverAction = action;
    st.actionEnforced = enforced;
    st.action = action;
    st.actionChange = now();
    player.setAction(action);
}

void PlayerStateService::clearSpecialAction(IPlayer &player)
{
    setSpecialAction(player, SpecialAction_None, false);
}

bool PlayerStateService::consumePutSanction(State &st, TimePoint timeNow)
{
    if (st.pendingPut && timeNow - st.putAt <= SANCTION_WINDOW)
    {
        st.pendingPut = false;
        return true;
    }
    return false;
}

bool PlayerStateService::consumeSpectateSanction(State &st, bool target, TimePoint timeNow)
{
    if (st.pendingSpectate && st.spectateTarget == target && timeNow - st.spectateAt <= SANCTION_WINDOW)
    {
        st.pendingSpectate = false;
        return true;
    }
    return false;
}

PlayerStateService::StateOutcome PlayerStateService::onStateChange(IPlayer &player, PlayerState newState,
                                                                   PlayerState oldState, TimePoint timeNow)
{
    StateOutcome outcome;
    State &st = m_state[player.getID()];
    const PlayerState prev = st.state; // наша принятая правда, не oldState ядра

    switch (newState)
    {
    case PlayerState_EnterVehicleDriver:
    case PlayerState_EnterVehiclePassenger:
        st.enterStart = timeNow; // началась легальная фаза входа
        break;

    case PlayerState_Driver:
    case PlayerState_Passenger:
    {
        if (consumePutSanction(st, timeNow))
            break; // серверная посадка

        // Смена места внутри ТС (Driver <-> Passenger) — легальна.
        if (prev == PlayerState_Driver || prev == PlayerState_Passenger)
            break;

        const PlayerState requiredPhase = (newState == PlayerState_Driver) ? PlayerState_EnterVehicleDriver
                                                                           : PlayerState_EnterVehiclePassenger;
        if (prev != requiredPhase)
        {
            // Мгновенная посадка без фазы входа — driverSync без анимации.
            outcome.stateHack = true;
            outcome.detail = fmt::format("instant vehicle entry: state {} -> {}", static_cast<int>(prev),
                                         static_cast<int>(newState));
            break;
        }

        if (timeNow - st.enterStart < MIN_ENTER)
        {
            outcome.stateHack = true;
            outcome.detail = fmt::format(
                "vehicle entry too fast: {}ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(timeNow - st.enterStart).count());
            break;
        }

        // Дистанция до машины в момент посадки — против входа «через карту».
        if (m_location)
        {
            IPlayerVehicleData *vehicleData = queryExtension<IPlayerVehicleData>(player);
            IVehicle *vehicle = vehicleData ? vehicleData->getVehicle() : nullptr;
            if (vehicle)
            {
                const float dist =
                    glm::distance(m_location->getPosition(player.getID()), vehicle->getPosition());
                if (dist > ENTER_MAX_DIST)
                {
                    outcome.stateHack = true;
                    outcome.detail = fmt::format("remote vehicle entry: {:.0f}m", dist);
                }
            }
        }
        break;
    }

    case PlayerState_Spectating:
        if (!consumeSpectateSanction(st, true, timeNow))
        {
            // Ядро отбрасывает spectator-sync без серверного спектейта, но переход
            // без санкции всё равно фиксируем.
            outcome.stateHack = true;
            outcome.detail = "unsanctioned spectate";
        }
        break;

    case PlayerState_OnFoot:
        if (prev == PlayerState_Spectating && !consumeSpectateSanction(st, false, timeNow))
        {
            // Самовольный выход из спектейта (например, во время авторизации).
            outcome.stateHack = true;
            outcome.detail = "left spectate unsanctioned";
        }
        break;

    default:
        break; // Wasted/Spawned/None/ExitVehicle — легальны из любого состояния
    }

    // Принимаем всегда: ядро уже считает игрока в новом стейте, расходиться с ним
    // бессмысленно. Решение по нарушению — за античитом (журнал + порог).
    st.state = newState;
    return outcome;
}

PlayerStateService::ActionOutcome PlayerStateService::verifyAction(IPlayer &player, TimePoint timeNow)
{
    ActionOutcome outcome;
    State &st = m_state[player.getID()];

    const PlayerState state = st.state;
    if (state != PlayerState_OnFoot && state != PlayerState_Driver && state != PlayerState_Passenger)
        return outcome;

    const PlayerSpecialAction reported = player.getAction(); // заявление клиента

    if (st.actionEnforced)
    {
        if (reported == st.serverAction)
        {
            st.action = reported;
            return outcome;
        }
        if (timeNow - st.actionChange < ACTION_GRACE)
            return outcome; // клиент ещё применяет

        // Побег из принудительного экшена (снял наручники) — переустанавливаем.
        player.setAction(st.serverAction);
        st.actionChange = timeNow;
        outcome.actionHack = true;
        outcome.detail = fmt::format("escaped enforced action {} (reported {})",
                                     static_cast<int>(st.serverAction), static_cast<int>(reported));
        return outcome;
    }

    if (reported == st.action)
        return outcome; // без изменений

    // Джетпак легален только с серверной выдачи.
    if (reported == SpecialAction_Jetpack && st.serverAction != SpecialAction_Jetpack)
    {
        if (timeNow - st.actionChange < ACTION_GRACE)
            return outcome;

        player.setAction(SpecialAction_None);
        st.action = SpecialAction_None;
        st.actionChange = timeNow;
        outcome.actionHack = true;
        outcome.detail = "jetpack without server grant";
        return outcome;
    }

    // Остальное (присед, вход/выход, окончание выпивки...) — клиентская правда.
    // Серверная выдача считается истраченной: повторное самовольное включение
    // того же экшена (например, джетпака после легального снятия) уже не пройдёт.
    if (reported != st.serverAction)
        st.serverAction = SpecialAction_None;
    st.action = reported;
    return outcome;
}

void PlayerStateService::onSpawn(IPlayer &player)
{
    State &st = m_state[player.getID()];
    st.state = player.getState(); // граница сервиса: сырое чтение здесь легально
    st.action = SpecialAction_None;
    st.serverAction = SpecialAction_None;
    st.actionEnforced = false;
    st.actionChange = now();
    st.pendingPut = false;
}

void PlayerStateService::reset(int playerId)
{
    m_state[playerId] = State{};
}
