#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Server/Components/TextDraws/textdraws.hpp"
#include "types.hpp"
#include <array>
#include <functional>
#include <string>
#include <unordered_map>

class TextDrawSystem;

// Полный набор визуальных параметров textdraw. Значения по умолчанию дают
// читаемый белый текст шрифтом 1 без бокса.
struct TextDrawParams
{
    Vector2 letterSize{0.4f, 1.6f};
    Vector2 textSize{400.0f, 17.0f};
    TextDrawAlignmentTypes alignment = TextDrawAlignment_Left;
    Colour letterColour = Colour::White();
    bool box = false;
    Colour boxColour = Colour(0, 0, 0, 128);
    Colour backgroundColour = Colour(0, 0, 0, 255);
    int shadow = 0;
    int outline = 1;
    TextDrawStyle style = TextDrawStyle_1;
    bool proportional = true;
    bool selectable = false;
    int previewModel = 0; // только для style Preview
    Vector3 previewRotation{0.0f, 0.0f, 0.0f};
    float previewZoom = 1.0f;
    int previewVehicleColour1 = -1; // -1 — случайные цвета машины в превью
    int previewVehicleColour2 = -1;
};

// Сервис textdraw — единая точка создания глобальных и per-player текстдравов.
// Всё, что уходит клиенту, проходит валидацию: текст санитизируется (пустой,
// слишком длинный, непарные '~' ломают/крашат клиент), числовые параметры
// клампятся к безопасным диапазонам.
//
// Клики: системы вешают обработчик на конкретный textdraw через setClickHandler /
// setPlayerClickHandler; TextDrawSystem (единственный подписчик событий SDK)
// маршрутизирует клики сюда. Клик-RPC подделывается читом для любого id, поэтому
// обработчик вызывается только если textdraw показан этому игроку и selectable —
// «нажать» скрытую или некликабельную кнопку нельзя. Режим выбора beginSelection()
// принимает колбэк отмены (ESC) — он живёт до endSelection()/отмены/выхода игрока.
class TextDrawService final : public IService
{
    friend TextDrawSystem;

  public:
    using ClickHandler = std::function<void(IPlayer &)>;
    using CancelHandler = std::function<void(IPlayer &)>;

    static constexpr std::size_t MAX_TEXT_LENGTH = 1024;

    // --- валидация ---
    // Проверка текста без изменения: false + причина (utf-8), если текст в таком
    // виде отправлять нельзя. Удобно для редактора — показать игроку, что не так.
    static bool validateText(StringView text, std::string &error);
    // Безопасная версия текста: контрольные символы убраны, длина ограничена,
    // непарная '~' отброшена, пустой текст заменён на "_".
    static std::string sanitizeText(StringView text);
    // Параметры с клампом всех числовых полей к безопасным диапазонам.
    static TextDrawParams clampParams(const TextDrawParams &params);
    // Экранная позиция: NaN/inf -> 0, кламп к окрестности экрана 640x480
    // (запас за краями оставлен для анимаций выезда).
    static Vector2 clampPosition(Vector2 position);

    // Компонент textdraw загружен и сервис работоспособен.
    bool isAvailable() const;

    // --- глобальные textdraw (один на всех, видимость per-player) ---
    ITextDraw *create(Vector2 position, StringView text, const TextDrawParams &params = {});
    void destroy(int textDrawId);
    ITextDraw *get(int textDrawId);
    bool setText(int textDrawId, StringView text);
    void showForPlayer(IPlayer &player, int textDrawId);
    void hideForPlayer(IPlayer &player, int textDrawId);

    // --- per-player textdraw (живут в пуле игрока, умирают с ним) ---
    IPlayerTextDraw *createForPlayer(IPlayer &player, Vector2 position, StringView text,
                                     const TextDrawParams &params = {});
    void destroyForPlayer(IPlayer &player, int textDrawId);
    IPlayerTextDraw *getForPlayer(IPlayer &player, int textDrawId);
    bool setTextForPlayer(IPlayer &player, int textDrawId, StringView text);

    // Применить параметры (с клампом) к существующему textdraw. Не делает
    // restream — вызывающий сам решает, когда перерисовать.
    void applyParams(ITextDrawBase &textDraw, const TextDrawParams &params);
    // Снять текущие параметры с textdraw (для сохранения/дублирования).
    static TextDrawParams readParams(const ITextDrawBase &textDraw);

    // --- клики ---
    void setClickHandler(int textDrawId, ClickHandler handler);
    void clearClickHandler(int textDrawId);
    void setPlayerClickHandler(IPlayer &player, int textDrawId, ClickHandler handler);
    void clearPlayerClickHandler(IPlayer &player, int textDrawId);

    // --- режим выбора textdraw кликом ---
    void beginSelection(IPlayer &player, Colour highlight, CancelHandler onCancel = nullptr);
    void endSelection(IPlayer &player);
    bool isSelecting(IPlayer &player) const;

  private:
    // Вызываются только TextDrawSystem.
    void initialize(ITextDrawsComponent *textDraws);
    void handleClick(IPlayer &player, ITextDraw &textDraw);
    void handlePlayerClick(IPlayer &player, IPlayerTextDraw &textDraw);
    bool handleCancelSelection(IPlayer &player); // true — отмена обработана колбэком
    void resetPlayer(int playerId);

    struct Slot
    {
        std::unordered_map<int, ClickHandler> clickHandlers; // ключ — id per-player textdraw
        CancelHandler cancelHandler;                         // активен только в режиме выбора
    };

    ITextDrawsComponent *m_textDraws = nullptr;
    std::unordered_map<int, ClickHandler> m_globalClickHandlers; // ключ — id глобального textdraw
    std::array<Slot, MAX_PLAYERS> m_slots;
};
