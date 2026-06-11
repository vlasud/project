#pragma once

#include "Services/IService.h"
#include "types.hpp"
#include <string>
#include <unordered_map>

class TextLabelSystem;
class StreamerService;

// Сервис 3D-текстовых лейблов (вывески, подписи над входами, инфо-точки).
//
// Лейблы стримятся через StreamerService (per-player, без клиентских лимитов):
//
//   int id = m_labels.add("Магазин 24/7\n{FFFF00}Открыто", {x, y, z});
//   m_labels.setText(id, "Закрыто");   // живое обновление у тех, кто рядом
//   m_labels.remove(id);
//
// Текст валидируется: длина ограничена, управляющие символы (кроме переноса
// строки) вычищаются, пустой текст заменяется заглушкой. Лейблы — данные
// сервер -> клиент, клиентского ввода у них нет, абьюзить нечего.
class TextLabelService final : public IService
{
    friend TextLabelSystem;

  public:
    static constexpr std::size_t MAX_TEXT_LENGTH = 256;
    static constexpr float MIN_DRAW_DISTANCE = 1.0f;
    static constexpr float MAX_DRAW_DISTANCE = 200.0f;

    // Безопасный текст лейбла: длина, контрольные символы (кроме '\n').
    static std::string sanitizeText(StringView text);

    // Создать лейбл. drawDistance — дистанция отрисовки (радиус стрима берётся
    // с запасом поверх неё). testLOS — скрывать за стенами. Возвращает id или -1.
    int add(StringView text, const Vector3 &position, Colour colour = Colour::White(), float drawDistance = 30.0f,
            bool testLOS = false);
    void remove(int labelId);
    bool exists(int labelId) const;

    // Живые обновления — доезжают мгновенно тем, кому лейбл показан.
    bool setText(int labelId, StringView text);
    bool setColour(int labelId, Colour colour);

  private:
    struct Def
    {
        std::string text;
        Colour colour = Colour::White();
    };

    // Вызывается TextLabelSystem.
    void initialize(StreamerService *streamer);

    StreamerService *m_streamer = nullptr;
    std::unordered_map<int, Def> m_defs; // ключ — def id стримера
};
