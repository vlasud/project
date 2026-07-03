#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"

#include <algorithm>
#include <chrono>

const ScreenNoticeService::Layout &ScreenNoticeService::layout()
{
    // Единый макет попапа — экспорт /td-редактора (низ-право, шрифт 3, без бокса).
    // letterColour макета — лишь дефолт: реальный цвет каждого показа задаёт вызывающий.
    static const Layout instance = [] {
        Layout layout;
        layout.position = Vector2(624.0f, 333.0f);
        layout.params.alignment = TextDrawAlignment_Right;
        layout.params.style = TextDrawStyle_3;
        layout.params.letterSize = Vector2(0.625f, 2.5f);
        layout.params.textSize = Vector2(400.0f, 17.0f);
        layout.params.letterColour = Colour::White();
        layout.params.boxColour = Colour(0x00, 0x00, 0x00, 0x80);
        layout.params.backgroundColour = Colour(0x00, 0x00, 0x00, 0xFF);
        layout.params.box = false;
        layout.params.proportional = true;
        layout.params.selectable = false;
        layout.params.shadow = 0;
        layout.params.outline = 0;
        return layout;
    }();
    return instance;
}

void ScreenNoticeService::show(IPlayer &player, StringView text, Milliseconds duration, Colour colour)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    if (!m_textDrawService || !m_timerService)
    {
        return; // сервис не инициализирован (компонент textdraw/timer недоступен)
    }
    if (text.empty())
    {
        return; // пустой текст не показываем (sanitizeText отрисовал бы "_")
    }

    duration = std::clamp(duration, MIN_DISPLAY, MAX_DISPLAY);
    State &state = m_states[playerId];

    if (!state.shown)
    {
        present(player, state, text, colour, duration);
        return;
    }

    const Milliseconds elapsed =
        std::chrono::duration_cast<Milliseconds>(std::chrono::steady_clock::now() - state.shownAt);
    if (elapsed >= MIN_DISPLAY)
    {
        // Показано достаточно долго — новое сообщение заменяет сразу, без мигания.
        state.pending = {};
        present(player, state, text, colour, duration);
        return;
    }

    // Показано меньше MIN_DISPLAY — новое сообщение откладывается (глубина
    // очереди 1, последний вызов молча вытесняет предыдущий pending) и выйдет
    // на экран ровно на границе shownAt+MIN_DISPLAY, независимо от duration
    // текущего показа.
    state.pending.valid = true;
    state.pending.text = std::string(text);
    state.pending.colour = colour;
    state.pending.duration = duration;

    const Milliseconds remaining = MIN_DISPLAY - elapsed;
    m_timerService->cancel(state.timer);
    state.timer = m_timerService->setPlayerTimeout(player, std::max(remaining, Milliseconds(0)),
                                                    [this](IPlayer &p)
                                                    {
                                                        const int id = p.getID();
                                                        if (id < 0 || id >= MAX_PLAYERS)
                                                        {
                                                            return;
                                                        }
                                                        onTimer(p, m_states[id]);
                                                    });
}

void ScreenNoticeService::hide(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS || !m_timerService || !m_textDrawService)
    {
        return;
    }

    State &state = m_states[playerId];
    m_timerService->cancel(state.timer);
    state.pending = {};
    if (state.shown)
    {
        state.shown = false;
        if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, state.textDrawId))
        {
            td->hide();
        }
    }
}

// ------------------------------------------------------------------ private

void ScreenNoticeService::present(IPlayer &player, State &state, StringView text, Colour colour,
                                  Milliseconds duration)
{
    if (state.textDrawId < 0)
    {
        // Лениво создаём единственный per-player textdraw, дальше только
        // setColour+setText+show.
        IPlayerTextDraw *created = m_textDrawService->createForPlayer(player, layout().position, text, layout().params);
        if (!created)
        {
            return; // пул per-player textdraw исчерпан — попап тихо не показываем
        }
        state.textDrawId = created->getID();
        created->setColour(colour);
    }
    else
    {
        m_textDrawService->setTextForPlayer(player, state.textDrawId, text);
        if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, state.textDrawId))
        {
            // setColour сам клиенту ничего не шлёт — обязателен повторный show()
            // ниже, который пересылает textdraw целиком.
            td->setColour(colour);
        }
    }

    if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, state.textDrawId))
    {
        td->show();
    }

    state.shown = true;
    state.shownAt = std::chrono::steady_clock::now();
    state.pending = {};

    m_timerService->cancel(state.timer);
    state.timer = m_timerService->setPlayerTimeout(player, duration,
                                                    [this](IPlayer &p)
                                                    {
                                                        const int id = p.getID();
                                                        if (id < 0 || id >= MAX_PLAYERS)
                                                        {
                                                            return;
                                                        }
                                                        onTimer(p, m_states[id]);
                                                    });
}

void ScreenNoticeService::onTimer(IPlayer &player, State &state)
{
    if (state.pending.valid)
    {
        Pending pending = std::move(state.pending);
        state.pending = {};
        present(player, state, pending.text, pending.colour, pending.duration);
        return;
    }

    state.shown = false;
    if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, state.textDrawId))
    {
        td->hide();
    }
}

void ScreenNoticeService::initialize(TextDrawService *textDrawService, TimerService *timerService)
{
    m_textDrawService = textDrawService;
    m_timerService = timerService;
}

void ScreenNoticeService::resetPlayer(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    // Per-player textdraw умирает вместе с пулом игрока — destroy не нужен.
    // Таймер setPlayerTimeout сам отменяется на дисконнекте (TimerService).
    // Сброс состояния, чтобы переиспользованный id игрока не унаследовал чужой
    // textdraw id/pending.
    m_states[playerId] = {};
}
