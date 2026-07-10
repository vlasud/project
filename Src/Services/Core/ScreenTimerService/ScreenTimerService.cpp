#include "Services/Core/ScreenTimerService/ScreenTimerService.h"

#include <fmt/format.h>
#include <string>

namespace
{
// Числовой остаток вида M:SS (языко-нейтрально), опц. англ. метка перед ним.
std::string formatTimer(int remainingSeconds, StringView label)
{
    const int minutes = remainingSeconds / 60;
    const int seconds = remainingSeconds % 60;
    std::string text;
    if (!label.empty())
    {
        text.append(label.data(), label.size());
        text.push_back(' ');
    }
    text += fmt::format("{}:{:02d}", minutes, seconds);
    return text;
}
} // namespace

const ScreenTimerService::Layout &ScreenTimerService::layout()
{
    // Пресет геймдизайнера из /td-редактора (Server/textdraws/timer.txt), поля — по
    // формату сериализации TextDrawEditorSystem. Верх-центр, шрифт 3 (Pricedown),
    // выравнивание по центру, белый текст. В пресете box ВЫКЛЮЧЕН (подложки нет);
    // боксовый/фоновый цвет — дефолт пресета (инертны при выключенном боксе).
    // outline=1 (расхождение с пресетом, box держим off): чёрный контур глифов даёт
    // контраст на светлом небе, где белый бар без подложки терялся бы.
    static const Layout instance = [] {
        Layout layout;
        layout.position = Vector2(320.0f, 114.0f);
        layout.params.alignment = TextDrawAlignment_Center;
        layout.params.style = TextDrawStyle_3;
        layout.params.letterSize = Vector2(0.7031f, 2.8125f);
        layout.params.textSize = Vector2(400.0f, 17.0f);
        layout.params.letterColour = Colour::White();
        layout.params.box = false;
        layout.params.boxColour = Colour(0x00, 0x00, 0x00, 0x80);
        layout.params.backgroundColour = Colour(0x00, 0x00, 0x00, 0xFF);
        layout.params.proportional = true;
        layout.params.selectable = false;
        layout.params.shadow = 0;
        layout.params.outline = 1;
        return layout;
    }();
    return instance;
}

void ScreenTimerService::show(IPlayer &player, int remainingSeconds, StringView label)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS || !m_textDrawService)
    {
        return;
    }
    // Защитный авто-hide: бизнес запушил истёкший остаток — гасим бар.
    if (remainingSeconds <= 0)
    {
        hide(player);
        return;
    }

    State &state = m_states[playerId];
    const std::string text = formatTimer(remainingSeconds, label);

    if (state.textDrawId < 0)
    {
        // Лениво создаём единственный per-player textdraw и показываем.
        IPlayerTextDraw *created =
            m_textDrawService->createForPlayer(player, layout().position, text, layout().params);
        if (!created)
        {
            return; // пул per-player textdraw исчерпан — молча не показываем
        }
        state.textDrawId = created->getID();
        created->show();
        state.shown = true;
        return;
    }

    if (!state.shown)
    {
        // Бар был скрыт — пере-показываем целиком: setText кладёт строку в память
        // (клиенту пока не шлёт, textdraw скрыт), show() пересылает textdraw с ней.
        m_textDrawService->setTextForPlayer(player, state.textDrawId, text);
        if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, state.textDrawId))
        {
            td->show();
        }
        state.shown = true;
        return;
    }

    // Бар уже на экране — лёгкое обновление ТОЛЬКО строки (SetString RPC), без
    // рестрима и без мигания (свойства макета не трогаем).
    m_textDrawService->setTextForPlayer(player, state.textDrawId, text);
}

void ScreenTimerService::hide(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS || !m_textDrawService)
    {
        return;
    }
    State &state = m_states[playerId];
    if (!state.shown)
    {
        return;
    }
    state.shown = false;
    if (IPlayerTextDraw *td = m_textDrawService->getForPlayer(player, state.textDrawId))
    {
        td->hide();
    }
}

void ScreenTimerService::initialize(TextDrawService *textDrawService)
{
    m_textDrawService = textDrawService;
}

void ScreenTimerService::resetPlayer(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    // Per-player textdraw умирает вместе с пулом игрока — destroy не нужен.
    // Сброс, чтобы переиспользованный id не унаследовал чужой textdraw id/флаг.
    m_states[playerId] = {};
}
