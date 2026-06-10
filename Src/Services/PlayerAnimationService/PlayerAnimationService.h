#pragma once

#include "../../Macro.h"
#include "../IService.h"
#include "anim.hpp"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <string>

// Сервис анимаций — единственный авторитетный источник «какую анимацию сервер
// выставил игроку». Геттеры isPlaying() возвращают true только для анимаций,
// инициированных сервером и подтверждённых синхронизацией клиента, поэтому
// игровая логика не может быть обманута клиентом, который врёт о своей анимации.
//
// Прерываемая (interruptible) анимация: игрок волен из неё выйти — когда это
// происходит, сервер просто перестаёт считать её активной.
// Непрерываемая (non-interruptible): если клиент вышел из анимации, сервер
// переустанавливает её (анти-чит). Такие анимации должны быть зацикленными или с
// freeze — снимаются только через stop().
class PlayerAnimationService final : public IService
{
  public:
    // Выставить анимацию игроку от имени сервера.
    void play(IPlayer &player, const AnimationData &animation, bool interruptible);

    // Снять серверную анимацию (и очистить её на клиенте).
    void stop(IPlayer &player);

    // Честность: проигрывает ли игрок прямо сейчас анимацию, выставленную сервером
    // и подтверждённую синхронизацией. Спуфнутая клиентом анимация, которую сервер
    // не выставлял, сюда не попадёт.
    bool isPlaying(int playerId) const;
    bool isPlaying(int playerId, StringView lib, StringView name) const;

    // Результат сверки: нарушение фиксируется только когда игрок вышел из
    // непрерываемой серверной анимации. detail заполняется лишь при нарушении.
    struct VerifyOutcome
    {
        bool forcedAnimationEscaped = false;
        std::string detail;
    };

    // Вызывается PlayerAnimationSystem.
    VerifyOutcome verify(IPlayer &player, TimePoint now);
    void reset(int playerId);

  private:
    struct State
    {
        bool active = false;       // сервер выставил анимацию
        bool interruptible = true; // можно ли игроку из неё выйти
        bool confirmed = false;    // клиент подтвердил, что проигрывает её
        AnimationData data;        // полные данные — для сверки и переустановки
        TimePoint appliedAt;       // когда последний раз применили (окно синхронизации)
    };

    std::array<State, MAX_PLAYERS> m_state;
};
