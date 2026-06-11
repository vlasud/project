#pragma once

#include "Services/IService.h"
#include "types.hpp"
#include <string>

// Политика валидации ника. Применяется NicknameSystem на входящем подключении
// (до onPlayerConnect); же API пригодится будущему /rename.
//
// Формат — строго Имя_Фамилия ("Lo_Vlasud"):
//  * только латиница и один разделитель '_';
//  * обе части минимум из 2 букв, с заглавной, дальше строчные;
//  * общая длина 5..20 (классический клиент стабильно рисует до 20).
//
// Дубликат ника отклоняет само ядро ("Nick name in use") — сюда он не доходит.
class NicknameService final : public IService
{
  public:
    static constexpr std::size_t MIN_LENGTH = 5; // Xx_Yy
    static constexpr std::size_t MAX_LENGTH = 20;

    enum class Verdict
    {
        Ok,
        TooShort,
        TooLong,
        BadCharacter, // символ вне латиницы и '_'
        BadFormat,    // структура не Имя_Фамилия
    };

    Verdict validate(StringView name) const;

    // Причина отказа для сообщения игроку (utf-8, конвертацию делает вызывающий).
    static const char *describe(Verdict verdict);
};
