#pragma once

#include "Macro.h"
#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>
#include <string>

class ScreenNoticeSystem;
struct IPlayer;

// Сервис экранных попапов поверх textdraw — замена native GameText, который в
// этом сетапе неуправляем (компонент Fixes на сервере отсутствует: сырой RPC
// sendGameText уходит клиенту, тот проигрывает СВОЮ fade-анимацию ~6с и не
// гасится hideGameText раньше срока). Попап — ОДИН per-player textdraw на
// игрока (единая экранная область под все виды попапов), с серверным таймером
// гашения и минимальным временем показа.
//
// Макет — фиксированный (см. ScreenNoticeService.cpp), letterColour макета —
// лишь дефолт: реальный цвет каждого показа задаётся параметром show().
class ScreenNoticeService final : public IService
{
    friend ScreenNoticeSystem;

  public:
    // Любое показанное сообщение остаётся на экране не меньше этого времени,
    // прежде чем его сменит следующее (не мигаем чужими короткими попапами).
    static constexpr Milliseconds MIN_DISPLAY{1000};
    static constexpr Milliseconds MAX_DISPLAY{60000};

    // Показать попап игроку на duration (клампится к [MIN_DISPLAY, MAX_DISPLAY]).
    // Ничего не показано — показывается сразу. Показано >= MIN_DISPLAY — заменяется
    // немедленно (без мигания). Показано < MIN_DISPLAY — новое сообщение становится
    // отложенным (глубина очереди 1, последний вызов побеждает) и выходит на экран
    // ровно в момент истечения MIN_DISPLAY текущего показа. Текст санитизирует
    // TextDrawService; пустой после санитизации не показываем.
    void show(IPlayer &player, StringView text, Milliseconds duration, Colour colour = Colour::White());
    // Скрыть попап немедленно, очистить отложенное сообщение и все таймеры.
    void hide(IPlayer &player);

  private:
    // Вызывается только ScreenNoticeSystem.
    void initialize(TextDrawService *textDrawService, TimerService *timerService);
    void resetPlayer(int playerId);

    struct Layout
    {
        Vector2 position;
        TextDrawParams params;
    };
    static const Layout &layout();

    struct Pending
    {
        bool valid = false;
        std::string text;
        Colour colour = Colour::White();
        Milliseconds duration{0};
    };

    struct State
    {
        int textDrawId = -1; // -1 — ещё не создан (пул per-player textdraw лениво)
        bool shown = false;  // сейчас есть видимый текст на экране
        TimePoint shownAt;   // момент последнего фактического показа
        // Единственный таймер состояния: пока pending пуст — это гашение текущего
        // показа по его duration; как только появляется pending — таймер
        // перепланирован на границу shownAt+MIN_DISPLAY (свап на pending), сам
        // duration текущего показа с этого момента уже не важен.
        TimerService::Handle timer;
        Pending pending;
    };

    // Реально вывести text/colour на экран поверх текущего (лениво создаёт
    // textdraw при первом показе), сбросить pending и завести таймер на duration.
    void present(IPlayer &player, State &state, StringView text, Colour colour, Milliseconds duration);
    // Сработка таймера состояния: показать pending, если есть, иначе погасить.
    void onTimer(IPlayer &player, State &state);

    TextDrawService *m_textDrawService = nullptr;
    TimerService *m_timerService = nullptr;
    std::array<State, MAX_PLAYERS> m_states;
};
