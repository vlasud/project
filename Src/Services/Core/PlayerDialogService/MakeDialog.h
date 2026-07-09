#pragma once

#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Utils/Encoding/Encoding.h"
#include <string>
#include <utility>

// Сборка Dialog из utf-8 полей: title/body/leftButton/rightButton прогоняются
// через u() (utf8Tocp1251) — исходники в utf-8, клиент SA-MP/open.mp рендерит
// кириллицу в cp1251.
inline Dialog makeDialog(DialogStyle style, const std::string &title, const std::string &body,
                         const std::string &leftButton, const std::string &rightButton)
{
    Dialog dialog;
    dialog.style = style;
    dialog.title = u(title);
    dialog.body = u(body);
    dialog.leftButton = u(leftButton);
    dialog.rightButton = u(rightButton);
    return dialog;
}

// Как makeDialog, но body уже в cp1251 (например, сырой текст textdraw или ввод
// игрока, собранный через u() по месту сборки): повторно конвертировать его
// нельзя — принимаем по значению и перемещаем как есть. Остальные поля — utf-8,
// через u().
inline Dialog makeDialogCp1251Body(DialogStyle style, const std::string &title, std::string body,
                                   const std::string &leftButton, const std::string &rightButton)
{
    Dialog dialog;
    dialog.style = style;
    dialog.title = u(title);
    dialog.body = std::move(body);
    dialog.leftButton = u(leftButton);
    dialog.rightButton = u(rightButton);
    return dialog;
}
