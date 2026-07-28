#pragma once

#include "Macro.h"
#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <string>

class ScreenNoticeSystem;
struct IPlayer;

// Сервис экранных попапов поверх textdraw — замена native GameText, который в
// этом сетапе неуправляем (компонент Fixes на сервере отсутствует: сырой RPC
// sendGameText уходит клиенту, тот проигрывает СВОЮ fade-анимацию ~6с и не
// гасится hideGameText раньше срока).
//
// ЛЕСЕНКА per-player textdraw на игрока: ОСНОВНОЙ (текущее сообщение) плюс
// PREVIEW_COUNT предпросмотров очереди — каждый следующий мельче и прозрачнее
// предыдущего, уходя «вглубь» экрана. Игрок заранее видит, что попапов
// накопилось и сколько; когда основной догорает, вся лесенка сдвигается на шаг
// вперёд: голова очереди выходит на основной слот, остальные подтягиваются.
//
// Очередь ограничена QUEUE_LIMIT: при переполнении выбрасывается САМОЕ СТАРОЕ
// ожидающее сообщение — попап показывает прогресс, и свежее всегда важнее.
//
// Макеты фиксированы (см. ScreenNoticeService.cpp), letterColour макета — лишь
// дефолт: реальный цвет каждого показа задаётся параметром show().
class ScreenNoticeService final : public IService
{
    friend ScreenNoticeSystem;

  public:
    // Любое показанное сообщение остаётся на экране не меньше этого времени,
    // прежде чем его сменит следующее (не мигаем чужими короткими попапами).
    // Оно же — шаг разбора очереди: пока в ней кто-то ждёт, каждое сообщение
    // держит основной слот ровно MIN_DISPLAY, независимо от своей duration.
    static constexpr Milliseconds MIN_DISPLAY{2000};
    static constexpr Milliseconds MAX_DISPLAY{60000};

    // Глубина очереди ожидающих сообщений (сверх показанного). Больше — попап
    // отставал бы от событий: игрок читал бы то, что случилось полминуты назад.
    static constexpr std::size_t QUEUE_LIMIT = 5;

    // Сколько ожидающих сообщений видно позади основного (лесенка предпросмотра).
    static constexpr std::size_t PREVIEW_COUNT = 2;

    // Показать попап игроку на duration (клампится к [MIN_DISPLAY, MAX_DISPLAY]).
    // Ничего не показано — выходит на экран сразу. Иначе встаёт в очередь: сразу
    // виден в фоновом слоте (если он первый в очереди) и займёт основной, когда
    // текущее сообщение отвисит свои MIN_DISPLAY. Текст санитизирует
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
    static const Layout &layout();                       // основной слот
    static const Layout &previewLayout(std::size_t level); // level 0 — ближний предпросмотр

    struct Pending
    {
        bool valid = false;
        std::string text;
        Colour colour = Colour::White();
        Milliseconds duration{0};
    };

    struct State
    {
        int textDrawId = -1; // основной слот; -1 — ещё не создан (лениво)
        std::array<int, PREVIEW_COUNT> previewId{-1, -1};
        bool shown = false; // сейчас есть видимый текст на основном слоте
        std::array<bool, PREVIEW_COUNT> previewShown{};
        TimePoint shownAt; // момент последнего фактического показа основного
        // Единственный таймер состояния: очередь пуста — это гашение текущего
        // показа по его duration; очередь не пуста — таймер стоит на границе
        // shownAt+MIN_DISPLAY (переезд следующего на основной слот).
        TimerService::Handle timer;
        std::deque<Pending> queue;
    };

    // Реально вывести text/colour на основной слот (лениво создаёт textdraw при
    // первом показе) и завести таймер: duration либо MIN_DISPLAY, если кто-то ждёт.
    void present(IPlayer &player, State &state, StringView text, Colour colour, Milliseconds duration);
    // Привести лесенку предпросмотра в соответствие с очередью (показать/сменить/погасить).
    void refreshPreviews(IPlayer &player, State &state);
    // Перепланировать таймер основного слота под текущее состояние очереди.
    void rescheduleTimer(IPlayer &player, State &state, Milliseconds delay);
    // Сработка таймера: поднять голову очереди на основной слот, иначе погасить.
    void onTimer(IPlayer &player, State &state);

    TextDrawService *m_textDrawService = nullptr;
    TimerService *m_timerService = nullptr;
    std::array<State, MAX_PLAYERS> m_states;
};
