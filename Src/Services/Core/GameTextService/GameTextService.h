#pragma once

#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <string>

class GameTextSystem;
struct ICore;

// Сервис экранных надписей GameText (стилизованные сообщения по центру/низу
// экрана: «Время вышло», название зоны, итог работы и т.п.).
//
//   m_gameText.show(player, u("~g~Заказ доставлен!"), Milliseconds(3000), 5);
//   m_gameText.showToAll(u("~r~Событие началось"), Milliseconds(4000), 0);
//   m_gameText.showNear(pos, 50.0f, u("..."), Milliseconds(3000), 5);
//   m_gameText.hide(player, 5);
//
// Текст — уже в cp1251 (литералы конвертируй Encoding::utf8Tocp1251). Стили
// 0..15 — независимые слоты open.mp (классические SA: 0..6). Валидация как у
// текстдравов: длина, управляющие символы, парность '~' — gametext использует
// тот же тильда-движок клиента, и непарная '~' для него опасна. Данные
// сервер -> клиент, клиентского ввода нет.
//
// Погасание НЕ гарантировано серверным time на всех стилях/клиентах (см.
// Docs/ScreenNotice.md) — для попапов, которым нужна надёжная длительность и
// приоритет нового показа, используется ScreenNoticeService (textdraw), не этот
// сервис.
class GameTextService final : public IService
{
    friend GameTextSystem;

  public:
    static constexpr int MAX_STYLE = 15;
    static constexpr std::size_t MAX_TEXT_LENGTH = 256;

    // Безопасный текст: обрезка, чистка управляющих, починка непарной '~'.
    static std::string sanitizeText(StringView text);

    // Пустой (после санитизации) текст не отправляется.
    void show(IPlayer &player, StringView text, Milliseconds time, int style);
    void showToAll(StringView text, Milliseconds time, int style);
    void showNear(const Vector3 &position, float radius, StringView text, Milliseconds time, int style);

    void hide(IPlayer &player, int style);
    void hideForAll(int style);
    bool isVisible(IPlayer &player, int style) const;

  private:
    // Вызывается GameTextSystem.
    void initialize(ICore *core);

    static int clampStyle(int style);
    static Milliseconds clampTime(Milliseconds time);

    ICore *m_core = nullptr;
};
