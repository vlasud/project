#include "Services/Core/TextLabelService/TextLabelService.h"

#include "Log/LogManager.h"
#include "Services/Core/StreamerService/StreamerService.h"
#include "Utils/Sanitize.h"
#include <algorithm>
#include <cmath>

namespace
{
// Запас радиуса стрима поверх дистанции отрисовки: лейбл уже создан у клиента,
// когда тот входит в зону видимости.
constexpr float STREAM_MARGIN = 30.0f;

} // namespace

std::string TextLabelService::sanitizeText(StringView text)
{
    std::string result;
    result.reserve(std::min(text.size(), MAX_TEXT_LENGTH));

    for (char c : text)
    {
        if (result.size() >= MAX_TEXT_LENGTH)
        {
            break;
        }
        // Перенос строки легален (лейблы многострочные), остальной контроль — нет.
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\n')
        {
            result += c;
        }
    }

    if (result.empty())
    {
        result = " "; // пустой текст не отправляем
    }
    return result;
}

int TextLabelService::add(StringView text, const Vector3 &position, Colour colour, float drawDistance, bool testLOS)
{
    if (!m_streamer)
    {
        LogManager::log(Error, "TextLabelService: not initialized, label dropped");
        return -1;
    }

    Def def;
    def.text = sanitizeText(text);
    def.colour = colour;

    const float draw = std::clamp(Utils::finiteOrZero(drawDistance), MIN_DRAW_DISTANCE, MAX_DRAW_DISTANCE);
    const Vector3 pos{Utils::finiteOrZero(position.x), Utils::finiteOrZero(position.y), Utils::finiteOrZero(position.z)};

    const int labelId = m_streamer->addTextLabel(def.text, def.colour, pos, draw, testLOS, draw + STREAM_MARGIN);
    if (labelId < 0)
    {
        LogManager::log(Warning, "TextLabelService: streamer rejected label");
        return -1;
    }

    m_defs[labelId] = std::move(def);
    return labelId;
}

void TextLabelService::remove(int labelId)
{
    if (m_defs.erase(labelId) > 0 && m_streamer)
    {
        m_streamer->removeTextLabel(labelId);
    }
}

bool TextLabelService::exists(int labelId) const
{
    return m_defs.find(labelId) != m_defs.end();
}

bool TextLabelService::setText(int labelId, StringView text)
{
    auto it = m_defs.find(labelId);
    if (it == m_defs.end() || !m_streamer)
    {
        return false;
    }
    it->second.text = sanitizeText(text);
    return m_streamer->updateTextLabel(labelId, it->second.text, it->second.colour);
}

bool TextLabelService::setColour(int labelId, Colour colour)
{
    auto it = m_defs.find(labelId);
    if (it == m_defs.end() || !m_streamer)
    {
        return false;
    }
    it->second.colour = colour;
    return m_streamer->updateTextLabel(labelId, it->second.text, it->second.colour);
}

void TextLabelService::initialize(StreamerService *streamer)
{
    m_streamer = streamer;
}
