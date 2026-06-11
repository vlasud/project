#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

class PlayerKeySystem;

// Сервис клавиш: подписки на нажатие/отпускание с детекцией фронта.
//
//   m_keys.onPress(PlayerKeyService::Key::Yes, [this](IPlayer &p) {
//       // игрок нажал Y — обработчик сам проверяет контекст (рядом ли дверь)
//   });
//   m_keys.isPressed(playerId, PlayerKeyService::Key::Sprint); // текущее состояние
//
// Особенности и честность:
//  * клавиши — клиентский ввод, «валидировать» их нельзя по определению;
//    обработчики обязаны сами проверять игровой контекст (дистанцию, стейт);
//  * чит может дёргать бит клавиши каждый синк — повторные нажатия одной
//    клавиши чаще KEY_DEBOUNCE отбрасываются (человек так не печатает),
//    поэтому спам бизнес-обработчиков ограничен;
//  * стрелки/WASD (upDown/leftRight) — не битовые клавиши и сюда не входят,
//    их читают напрямую из getKeyData() (как делают редакторы).
class PlayerKeyService final : public IService
{
    friend PlayerKeySystem;

  public:
    // Битовые значения PlayerKeyData::keys (классические значения SA).
    enum Key : std::uint32_t
    {
        Action = 1,
        Crouch = 2,
        Fire = 4,
        Sprint = 8,
        SecondaryAttack = 16, // вход в машину / влезть
        Jump = 32,
        LookRight = 64,
        Handbrake = 128, // прицел на ногах
        LookLeft = 256,
        Submission = 512, // клаксон / каподастр сабмишна
        Walk = 1024,
        AnalogUp = 2048,
        AnalogDown = 4096,
        AnalogLeft = 8192,
        AnalogRight = 16384,
        Yes = 65536,
        No = 131072,
        CtrlBack = 262144,
    };

    using Handler = std::function<void(IPlayer &)>;

    // Подписка на нажатие (фронт) клавиши. Возвращает id подписки.
    int onPress(std::uint32_t key, Handler handler);
    // Подписка на отпускание.
    int onRelease(std::uint32_t key, Handler handler);
    void unsubscribe(int subscriptionId);

    // Текущее состояние клавиши по последнему синку игрока.
    bool isPressed(int playerId, std::uint32_t key) const;

  private:
    struct Subscription
    {
        int id = 0;
        std::uint32_t key = 0;
        bool press = true; // false — отпускание
        Handler handler;
    };

    // Вызываются PlayerKeySystem.
    void handleKeyStateChange(IPlayer &player, std::uint32_t newKeys, std::uint32_t oldKeys, TimePoint now);
    void resetPlayer(int playerId);

    void dispatch(IPlayer &player, std::uint32_t key, bool press);

    std::vector<Subscription> m_subscriptions;
    int m_nextId = 1;

    std::array<std::uint32_t, MAX_PLAYERS> m_lastKeys{};
    // антиспам: последнее принятое нажатие клавиши per-player per-key
    std::array<std::unordered_map<std::uint32_t, TimePoint>, MAX_PLAYERS> m_lastPress;

    // Переиспользуемый буфер диспатча: обработчик может подписываться/
    // отписываться из колбэка, поэтому зовём по снапшоту.
    std::vector<Handler> m_dispatchScratch;
};
