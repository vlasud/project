#include "Services/Core/NicknameService/NicknameService.h"

namespace
{
bool isUpper(char c)
{
    return c >= 'A' && c <= 'Z';
}

bool isLower(char c)
{
    return c >= 'a' && c <= 'z';
}

// Часть имени: минимум 2 буквы, заглавная + строчные ("Lo", "Vlasud").
bool isNamePart(StringView part)
{
    if (part.size() < 2 || !isUpper(part[0]))
        return false;
    for (std::size_t i = 1; i < part.size(); ++i)
    {
        if (!isLower(part[i]))
            return false;
    }
    return true;
}
} // namespace

NicknameService::Verdict NicknameService::validate(StringView name) const
{
    if (name.size() < MIN_LENGTH)
        return Verdict::TooShort;
    if (name.size() > MAX_LENGTH)
        return Verdict::TooLong;

    std::size_t underscore = StringView::npos;
    for (std::size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        if (c == '_')
        {
            if (underscore != StringView::npos)
                return Verdict::BadFormat; // второй разделитель
            underscore = i;
        }
        else if (!isUpper(c) && !isLower(c))
        {
            return Verdict::BadCharacter;
        }
    }
    if (underscore == StringView::npos)
        return Verdict::BadFormat;

    if (!isNamePart(name.substr(0, underscore)) || !isNamePart(name.substr(underscore + 1)))
        return Verdict::BadFormat;
    return Verdict::Ok;
}

const char *NicknameService::describe(Verdict verdict)
{
    switch (verdict)
    {
    case Verdict::TooShort:
        return "ник короче 5 символов";
    case Verdict::TooLong:
        return "ник длиннее 20 символов";
    case Verdict::BadCharacter:
        return "в нике допустимы только латинские буквы и один символ _";
    case Verdict::BadFormat:
        return "формат ника — Имя_Фамилия";
    case Verdict::Ok:
        break;
    }
    return "";
}
