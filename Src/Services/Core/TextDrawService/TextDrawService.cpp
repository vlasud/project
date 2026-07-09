#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Log/LogManager.h"
#include "Utils/Sanitize.h"
#include <algorithm>
#include <cmath>

namespace
{
// Экран SA — виртуальные 640x480; запас за краями оставлен для анимаций выезда.
constexpr float MIN_SCREEN_X = -640.0f;
constexpr float MAX_SCREEN_X = 1280.0f;
constexpr float MIN_SCREEN_Y = -480.0f;
constexpr float MAX_SCREEN_Y = 960.0f;

constexpr float MAX_LETTER_SIZE = 10.0f;
constexpr float MAX_TEXT_SIZE = 640.0f;
constexpr int MAX_SHADOW = 24;
constexpr int MAX_OUTLINE = 24;
constexpr int MAX_PREVIEW_MODEL = 20000;
constexpr float MIN_PREVIEW_ZOOM = 0.05f;
constexpr float MAX_PREVIEW_ZOOM = 10.0f;
constexpr int MAX_VEHICLE_COLOUR = 255;
} // namespace

// ------------------------------------------------------------------ валидация

bool TextDrawService::validateText(StringView text, std::string &error)
{
    if (text.empty())
    {
        error = "текст пуст";
        return false;
    }
    if (text.size() > MAX_TEXT_LENGTH)
    {
        error = "текст длиннее " + std::to_string(MAX_TEXT_LENGTH) + " символов";
        return false;
    }

    std::size_t tildes = 0;
    for (char c : text)
    {
        if (c == '~')
        {
            ++tildes;
        }
        else if (static_cast<unsigned char>(c) < 0x20)
        {
            error = "текст содержит управляющие символы";
            return false;
        }
    }
    if (tildes % 2 != 0)
    {
        error = "непарный символ '~' (коды вида ~r~ должны быть закрыты)";
        return false;
    }

    return true;
}

std::string TextDrawService::sanitizeText(StringView text)
{
    std::string result;
    result.reserve(std::min(text.size(), MAX_TEXT_LENGTH));

    for (char c : text)
    {
        if (result.size() >= MAX_TEXT_LENGTH)
        {
            break;
        }
        if (static_cast<unsigned char>(c) >= 0x20)
        {
            result += c;
        }
    }

    // Непарная '~' ломает разбор кодов на клиенте — убираем последнюю.
    if (std::count(result.begin(), result.end(), '~') % 2 != 0)
    {
        result.erase(result.rfind('~'), 1);
    }

    if (result.empty())
    {
        result = "_"; // пустой текст textdraw крашит клиент
    }
    return result;
}

TextDrawParams TextDrawService::clampParams(const TextDrawParams &params)
{
    TextDrawParams p = params;

    p.letterSize.x = Utils::clampFinite(p.letterSize.x, -MAX_LETTER_SIZE, MAX_LETTER_SIZE);
    p.letterSize.y = Utils::clampFinite(p.letterSize.y, -MAX_LETTER_SIZE, MAX_LETTER_SIZE);
    p.textSize.x = Utils::clampFinite(p.textSize.x, 0.0f, MAX_TEXT_SIZE);
    p.textSize.y = Utils::clampFinite(p.textSize.y, 0.0f, MAX_TEXT_SIZE);
    p.shadow = std::clamp(p.shadow, 0, MAX_SHADOW);
    p.outline = std::clamp(p.outline, 0, MAX_OUTLINE);

    if (p.style < TextDrawStyle_0 || p.style > TextDrawStyle_Preview)
    {
        p.style = TextDrawStyle_1;
    }
    if (p.alignment < TextDrawAlignment_Default || p.alignment > TextDrawAlignment_Right)
    {
        p.alignment = TextDrawAlignment_Left;
    }

    p.previewModel = std::clamp(p.previewModel, -1, MAX_PREVIEW_MODEL);
    p.previewRotation.x = Utils::finiteOrZero(p.previewRotation.x);
    p.previewRotation.y = Utils::finiteOrZero(p.previewRotation.y);
    p.previewRotation.z = Utils::finiteOrZero(p.previewRotation.z);
    p.previewZoom = Utils::clampFinite(p.previewZoom, MIN_PREVIEW_ZOOM, MAX_PREVIEW_ZOOM);
    p.previewVehicleColour1 = std::clamp(p.previewVehicleColour1, -1, MAX_VEHICLE_COLOUR);
    p.previewVehicleColour2 = std::clamp(p.previewVehicleColour2, -1, MAX_VEHICLE_COLOUR);

    return p;
}

Vector2 TextDrawService::clampPosition(Vector2 position)
{
    return {Utils::clampFinite(position.x, MIN_SCREEN_X, MAX_SCREEN_X), Utils::clampFinite(position.y, MIN_SCREEN_Y, MAX_SCREEN_Y)};
}

// ------------------------------------------------------------------ глобальные

bool TextDrawService::isAvailable() const
{
    return m_textDraws != nullptr;
}

ITextDraw *TextDrawService::create(Vector2 position, StringView text, const TextDrawParams &params)
{
    if (!m_textDraws)
    {
        LogManager::log(Error, "TextDrawService: ITextDrawsComponent is missing");
        return nullptr;
    }

    const TextDrawParams clamped = clampParams(params);
    const std::string safeText = sanitizeText(text);
    const Vector2 pos = clampPosition(position);

    ITextDraw *textDraw = (clamped.style == TextDrawStyle_Preview) ? m_textDraws->create(pos, clamped.previewModel)
                                                                   : m_textDraws->create(pos, safeText);
    if (!textDraw)
    {
        LogManager::log(Warning, "TextDrawService: failed to create textdraw (pool limit?)");
        return nullptr;
    }

    textDraw->setText(safeText);
    applyParams(*textDraw, clamped);
    return textDraw;
}

void TextDrawService::destroy(int textDrawId)
{
    m_globalClickHandlers.erase(textDrawId);
    if (m_textDraws && m_textDraws->get(textDrawId))
    {
        m_textDraws->release(textDrawId);
    }
}

ITextDraw *TextDrawService::get(int textDrawId)
{
    return m_textDraws ? m_textDraws->get(textDrawId) : nullptr;
}

bool TextDrawService::setText(int textDrawId, StringView text)
{
    ITextDraw *textDraw = get(textDrawId);
    if (!textDraw)
    {
        return false;
    }
    textDraw->setText(sanitizeText(text));
    return true;
}

void TextDrawService::showForPlayer(IPlayer &player, int textDrawId)
{
    if (ITextDraw *textDraw = get(textDrawId))
    {
        textDraw->showForPlayer(player);
    }
}

void TextDrawService::hideForPlayer(IPlayer &player, int textDrawId)
{
    if (ITextDraw *textDraw = get(textDrawId))
    {
        textDraw->hideForPlayer(player);
    }
}

// ------------------------------------------------------------------ per-player

IPlayerTextDraw *TextDrawService::createForPlayer(IPlayer &player, Vector2 position, StringView text,
                                                  const TextDrawParams &params)
{
    IPlayerTextDrawData *data = queryExtension<IPlayerTextDrawData>(player);
    if (!data)
    {
        LogManager::log(Error, "TextDrawService: IPlayerTextDrawData extension is missing");
        return nullptr;
    }

    const TextDrawParams clamped = clampParams(params);
    const std::string safeText = sanitizeText(text);
    const Vector2 pos = clampPosition(position);

    IPlayerTextDraw *textDraw = (clamped.style == TextDrawStyle_Preview) ? data->create(pos, clamped.previewModel)
                                                                         : data->create(pos, safeText);
    if (!textDraw)
    {
        LogManager::log(Warning, "TextDrawService: failed to create player textdraw (pool limit?)");
        return nullptr;
    }

    textDraw->setText(safeText);
    applyParams(*textDraw, clamped);
    return textDraw;
}

void TextDrawService::destroyForPlayer(IPlayer &player, int textDrawId)
{
    m_slots[player.getID()].clickHandlers.erase(textDrawId);
    if (IPlayerTextDrawData *data = queryExtension<IPlayerTextDrawData>(player))
    {
        if (data->get(textDrawId))
        {
            data->release(textDrawId);
        }
    }
}

IPlayerTextDraw *TextDrawService::getForPlayer(IPlayer &player, int textDrawId)
{
    IPlayerTextDrawData *data = queryExtension<IPlayerTextDrawData>(player);
    return data ? data->get(textDrawId) : nullptr;
}

bool TextDrawService::setTextForPlayer(IPlayer &player, int textDrawId, StringView text)
{
    IPlayerTextDraw *textDraw = getForPlayer(player, textDrawId);
    if (!textDraw)
    {
        return false;
    }
    textDraw->setText(sanitizeText(text));
    return true;
}

// ------------------------------------------------------------------ параметры

void TextDrawService::applyParams(ITextDrawBase &textDraw, const TextDrawParams &params)
{
    const TextDrawParams p = clampParams(params);

    textDraw.setLetterSize(p.letterSize);
    textDraw.setTextSize(p.textSize);
    textDraw.setAlignment(p.alignment);
    textDraw.setColour(p.letterColour);
    textDraw.useBox(p.box);
    textDraw.setBoxColour(p.boxColour);
    textDraw.setBackgroundColour(p.backgroundColour);
    textDraw.setShadow(p.shadow);
    textDraw.setOutline(p.outline);
    textDraw.setStyle(p.style);
    textDraw.setProportional(p.proportional);
    textDraw.setSelectable(p.selectable);
    textDraw.setPreviewModel(p.previewModel);
    textDraw.setPreviewRotation(p.previewRotation);
    textDraw.setPreviewZoom(p.previewZoom);
    if (p.previewVehicleColour1 >= 0 && p.previewVehicleColour2 >= 0)
    {
        textDraw.setPreviewVehicleColour(p.previewVehicleColour1, p.previewVehicleColour2);
    }
}

TextDrawParams TextDrawService::readParams(const ITextDrawBase &textDraw)
{
    TextDrawParams p;
    p.letterSize = textDraw.getLetterSize();
    p.textSize = textDraw.getTextSize();
    p.alignment = textDraw.getAlignment();
    p.letterColour = textDraw.getLetterColour();
    p.box = textDraw.hasBox();
    p.boxColour = textDraw.getBoxColour();
    p.backgroundColour = textDraw.getBackgroundColour();
    p.shadow = textDraw.getShadow();
    p.outline = textDraw.getOutline();
    p.style = textDraw.getStyle();
    p.proportional = textDraw.isProportional();
    p.selectable = textDraw.isSelectable();
    p.previewModel = textDraw.getPreviewModel();
    p.previewRotation = textDraw.getPreviewRotation();
    p.previewZoom = textDraw.getPreviewZoom();
    const Pair<int, int> vehicleColours = textDraw.getPreviewVehicleColour();
    p.previewVehicleColour1 = vehicleColours.first;
    p.previewVehicleColour2 = vehicleColours.second;
    return p;
}

// ------------------------------------------------------------------ клики и выбор

void TextDrawService::setClickHandler(int textDrawId, ClickHandler handler)
{
    m_globalClickHandlers[textDrawId] = std::move(handler);
}

void TextDrawService::clearClickHandler(int textDrawId)
{
    m_globalClickHandlers.erase(textDrawId);
}

void TextDrawService::setPlayerClickHandler(IPlayer &player, int textDrawId, ClickHandler handler)
{
    m_slots[player.getID()].clickHandlers[textDrawId] = std::move(handler);
}

void TextDrawService::clearPlayerClickHandler(IPlayer &player, int textDrawId)
{
    m_slots[player.getID()].clickHandlers.erase(textDrawId);
}

void TextDrawService::beginSelection(IPlayer &player, Colour highlight, CancelHandler onCancel)
{
    IPlayerTextDrawData *data = queryExtension<IPlayerTextDrawData>(player);
    if (!data)
    {
        LogManager::log(Error, "TextDrawService: IPlayerTextDrawData extension is missing");
        return;
    }

    m_slots[player.getID()].cancelHandler = std::move(onCancel);
    data->beginSelection(highlight);
}

void TextDrawService::endSelection(IPlayer &player)
{
    m_slots[player.getID()].cancelHandler = nullptr;
    if (IPlayerTextDrawData *data = queryExtension<IPlayerTextDrawData>(player))
    {
        data->endSelection();
    }
}

bool TextDrawService::isSelecting(IPlayer &player) const
{
    IPlayerTextDrawData *data = queryExtension<IPlayerTextDrawData>(player);
    return data && data->isSelecting();
}

// ------------------------------------------------------------------ вызовы TextDrawSystem

void TextDrawService::initialize(ITextDrawsComponent *textDraws)
{
    m_textDraws = textDraws;
}

void TextDrawService::handleClick(IPlayer &player, ITextDraw &textDraw)
{
    // Клик-RPC подделывается клиентом для любого id: честный клик возможен только
    // по textdraw, который показан этому игроку и помечен selectable.
    if (!textDraw.isShownForPlayer(player) || !textDraw.isSelectable())
    {
        return;
    }

    auto it = m_globalClickHandlers.find(textDraw.getID());
    if (it != m_globalClickHandlers.end() && it->second)
    {
        it->second(player);
    }
}

void TextDrawService::handlePlayerClick(IPlayer &player, IPlayerTextDraw &textDraw)
{
    if (!textDraw.isShown() || !textDraw.isSelectable())
    {
        return; // спуф: textdraw скрыт или не кликабелен
    }

    Slot &slot = m_slots[player.getID()];
    auto it = slot.clickHandlers.find(textDraw.getID());
    if (it != slot.clickHandlers.end() && it->second)
    {
        it->second(player);
    }
}

bool TextDrawService::handleCancelSelection(IPlayer &player)
{
    Slot &slot = m_slots[player.getID()];
    if (!slot.cancelHandler)
    {
        return false;
    }

    // Забираем колбэк до вызова: внутри него может быть новый beginSelection.
    CancelHandler handler = std::move(slot.cancelHandler);
    slot.cancelHandler = nullptr;
    handler(player);
    return true;
}

void TextDrawService::resetPlayer(int playerId)
{
    // Per-player текстдравы умирают вместе с пулом игрока — чистим только наше.
    m_slots[playerId].clickHandlers.clear();
    m_slots[playerId].cancelHandler = nullptr;
}
