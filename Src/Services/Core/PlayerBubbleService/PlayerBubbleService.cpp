#include "Services/Core/PlayerBubbleService/PlayerBubbleService.h"

#include "Utils/Encoding/Encoding.h"
#include "Utils/Sanitize.h"
#include <algorithm>
#include <chrono>

namespace
{
constexpr Milliseconds TIME_MIN{500};   // короче окружающие не успевают прочитать
constexpr Milliseconds TIME_MAX{60000}; // бабл — реплика, а не постоянная надпись над головой
constexpr float DRAW_DISTANCE_MIN = 1.0f;
constexpr float DRAW_DISTANCE_MAX = 100.0f;
} // namespace

std::string PlayerBubbleService::sanitizeText(StringView text)
{
    if (text.empty())
    {
        return {}; // у пустого view data() может быть nullptr — до конструктора не доводим
    }
    // Текст уже в cp1251: кодировка однобайтовая, поэтому рез по байту не разрывает
    // символ, а чистка идёт на месте.
    std::string result(text.data(), std::min(text.size(), MAX_TEXT_LENGTH));
    // Цветокоды ('{', '}', '~') и управляющие байты — одним проходом штатной чисткой
    // готовой cp1251-строки, уходящей клиенту.
    Encoding::neutralizeLine(result.data(), result.size());
    return result;
}

void PlayerBubbleService::show(IPlayer &player, StringView text, const Colour &colour, Milliseconds time,
                               float drawDistance)
{
    const std::string safe = sanitizeText(text);
    if (safe.empty())
    {
        return;
    }
    player.setChatBubble(safe, colour, Utils::clampFinite(drawDistance, DRAW_DISTANCE_MIN, DRAW_DISTANCE_MAX),
                         std::clamp(time, TIME_MIN, TIME_MAX));
}
