#pragma once

#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <cstddef>
#include <string>

// Чат-бабл над головой игрока: короткая строка, которую видят окружающие тем же
// движком, что и баблы чата.
//
//   m_bubble.show(player, u("Вернулся"), Colour::White(), Milliseconds(5000));
//
// Текст — уже в cp1251 (литералы через Encoding::utf8Tocp1251). Санитизация
// обязательна и делается ЗДЕСЬ штатной чисткой готовой строки
// (Encoding::neutralizeLine): цветокоды ('{', '}', '~') клиент интерпретирует при
// рендере бабла так же, как в client message, поэтому ник/игроковый текст без
// нейтрализации ломал бы отображение; управляющие байты обезвреживаются, длина
// режется по капу, длительность и дальность клампятся.
//
// Один бабл на игрока — новый показ заменяет предыдущий. Ядро само перевыдаёт
// активный бабл тем, кто заходит в стрим игрока, с остатком времени (см.
// player.cpp::streamInForPlayer) — повторять показ гейммоду не нужно.
class PlayerBubbleService final : public IService
{
  public:
    // Кап текста. По проводу строка уходит writeDynStr8 (потолок 255 байт), 128 —
    // проектный предел «реплика, а не простыня» (и inline-ёмкость HybridString в RPC).
    static constexpr std::size_t MAX_TEXT_LENGTH = 128;
    // Дальность видимости по умолчанию: на ней бабл ещё читаем. Собственный
    // дисплей-дефолт Core — на балансовые константы бизнес-фич не ссылается.
    static constexpr float DEFAULT_DRAW_DISTANCE = 20.0f;

    // Безопасный текст: обезвреженные цветокоды и управляющие байты, обрезка.
    static std::string sanitizeText(StringView text);

    // Пустой (после санитизации) текст не отправляется.
    void show(IPlayer &player, StringView text, const Colour &colour, Milliseconds time,
              float drawDistance = DEFAULT_DRAW_DISTANCE);
};
