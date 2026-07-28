#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"

#include <algorithm>
#include <chrono>
#include <utility>

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

namespace
{
// Шаги лесенки предпросмотра — экспорт /td-редактора. Каждый следующий мельче и
// прозрачнее и смещён выше-правее, поэтому очередь читается как уходящая вглубь.
// Альфа здесь — только дефолт макета: на показе она накладывается на ЦВЕТ
// СООБЩЕНИЯ, чтобы смысл цвета (зелёный доход, красный отказ) читался и в глубине.
struct PreviewStep
{
    Vector2 position;
    Vector2 letterSize;
    std::uint8_t alpha;
};
// const, а не constexpr: Vector2 в этом SDK не constexpr-конструируем (MSVC).
const PreviewStep PREVIEW_STEPS[] = {
    {Vector2(630.0f, 326.0f), Vector2(0.256f, 1.024f), 0x55},
    {Vector2(634.0f, 320.0f), Vector2(0.1638f, 0.6554f), 0x22},
};
static_assert(sizeof(PREVIEW_STEPS) / sizeof(PREVIEW_STEPS[0]) == ScreenNoticeService::PREVIEW_COUNT,
              "шагов лесенки должно быть ровно PREVIEW_COUNT");
} // namespace

const ScreenNoticeService::Layout &ScreenNoticeService::previewLayout(std::size_t level)
{
    static const std::array<Layout, PREVIEW_COUNT> instances = [] {
        std::array<Layout, PREVIEW_COUNT> layouts;
        for (std::size_t i = 0; i < PREVIEW_COUNT; ++i)
        {
            Layout &layout = layouts[i];
            layout.position = PREVIEW_STEPS[i].position;
            layout.params.alignment = TextDrawAlignment_Right;
            layout.params.style = TextDrawStyle_3;
            layout.params.letterSize = PREVIEW_STEPS[i].letterSize;
            layout.params.textSize = Vector2(400.0f, 17.0f);
            layout.params.letterColour = Colour(0xFF, 0xFF, 0xFF, PREVIEW_STEPS[i].alpha);
            layout.params.boxColour = Colour(0x00, 0x00, 0x00, 0x80);
            layout.params.backgroundColour = Colour(0x00, 0x00, 0x00, 0xFF);
            layout.params.box = false;
            layout.params.proportional = true;
            layout.params.selectable = false;
            layout.params.shadow = 0;
            layout.params.outline = 0;
        }
        return layouts;
    }();
    return instances[std::min(level, PREVIEW_COUNT - 1)];
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

    // Занято — в очередь. Переполнение выбрасывает САМОЕ СТАРОЕ ожидающее: попап
    // показывает прогресс, и свежее событие всегда важнее протухшего.
    Pending queued;
    queued.valid = true;
    queued.text = std::string(text);
    queued.colour = colour;
    queued.duration = duration;
    state.queue.push_back(std::move(queued));
    while (state.queue.size() > QUEUE_LIMIT)
    {
        state.queue.pop_front();
    }
    refreshPreviews(player, state);

    // Пока кто-то ждёт, основной слот держится ровно MIN_DISPLAY: иначе длинный
    // попап (duration до минуты) заморозил бы всю очередь позади себя.
    const Milliseconds elapsed =
        std::chrono::duration_cast<Milliseconds>(std::chrono::steady_clock::now() - state.shownAt);
    rescheduleTimer(player, state, elapsed >= MIN_DISPLAY ? Milliseconds(0) : MIN_DISPLAY - elapsed);
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
    state.queue.clear();
    if (state.shown)
    {
        state.shown = false;
        if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, state.textDrawId))
        {
            td->hide();
        }
    }
    refreshPreviews(player, state); // очередь пуста — фоновый слот гаснет
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

    // Очередь не пуста — держим слот ровно MIN_DISPLAY, иначе живём свою duration.
    rescheduleTimer(player, state, state.queue.empty() ? duration : MIN_DISPLAY);
}

void ScreenNoticeService::refreshPreviews(IPlayer &player, State &state)
{
    for (std::size_t level = 0; level < PREVIEW_COUNT; ++level)
    {
        int &slotId = state.previewId[level];
        bool &slotShown = state.previewShown[level];

        if (state.queue.size() <= level)
        {
            if (slotShown)
            {
                slotShown = false;
                if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, slotId))
                {
                    td->hide();
                }
            }
            continue;
        }

        const Pending &next = state.queue[level];
        // Цвет сообщения с альфой своего шага: чем глубже, тем прозрачнее, но смысл
        // цвета (зелёный доход, красный отказ) читается на любой глубине.
        const Layout &slotLayout = previewLayout(level);
        const Colour faded(next.colour.r, next.colour.g, next.colour.b, slotLayout.params.letterColour.a);

        if (slotId < 0)
        {
            IPlayerTextDraw *created =
                m_textDrawService->createForPlayer(player, slotLayout.position, next.text, slotLayout.params);
            if (!created)
            {
                continue; // пул per-player textdraw исчерпан — этот шаг не показываем
            }
            slotId = created->getID();
            created->setColour(faded);
        }
        else
        {
            m_textDrawService->setTextForPlayer(player, slotId, next.text);
            if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, slotId))
            {
                td->setColour(faded);
            }
        }

        if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, slotId))
        {
            td->show(); // setColour/setText клиенту сами не уходят — пересылаем целиком
        }
        slotShown = true;
    }
}

void ScreenNoticeService::rescheduleTimer(IPlayer &player, State &state, Milliseconds delay)
{
    m_timerService->cancel(state.timer);
    state.timer = m_timerService->setPlayerTimeout(player, std::max(delay, Milliseconds(0)),
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
    if (!state.queue.empty())
    {
        // Голова очереди переезжает из фона на основной слот, а в фон встаёт
        // следующая — порядок на экране остаётся тем, что видел игрок.
        Pending next = std::move(state.queue.front());
        state.queue.pop_front();
        refreshPreviews(player, state);
        present(player, state, next.text, next.colour, next.duration);
        return;
    }

    state.shown = false;
    if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, state.textDrawId))
    {
        td->hide();
    }
    refreshPreviews(player, state);
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
