# Логотип сервера «HARDWAY»

Постоянный логотип сервера — глобальный textdraw, видимый всем игрокам всё
время. Контент-фича (не Core).

Код: `Src/Systems/ServerLogoSystem/ServerLogoSystem.{h,cpp}`. Зависимость из
регистра сервисов — `TextDrawService` (core). Регистрация —
`Src/Systems/SystemRegister.cpp`, после `TextDrawSystem` (к моменту
`initialize()` сервис уже получил компонент textdraw).

## Механика

- **Глобальный textdraw** (один на всех): создаётся через
  `TextDrawService::create(...)`, не `createForPlayer`. Видимость управляется
  per-player через `showForPlayer`.
- **Создание — один раз** в `initialize(IComponentList*)`, при готовом
  компоненте textdraw (`isAvailable()`). Сохраняется `int m_logoId` =
  `td->getID()`. Логотип живёт всю сессию сервера, не уничтожается.
- **Показ** — в `onPlayerConnect`: если `m_logoId >= 0`, вызывается
  `showForPlayer`. На дисконнекте ничего не делаем — видимость глобального
  textdraw снимается с уходом игрока сама.
- **Не на тике**: подписки на `onPlayerUpdate` нет; показ — O(1) на подключение.

## Параметры (строка экспорта дев-тулзы `td`)

```
td 533.00 4.00 2 1 0.4000 1.6000 400.00 17.00 FFFFFFFF 00000080 000000FF 0 1 0 0 0 0 0.00 0.00 0.00 1.000 -1 -1 HARDWAY
```

| Параметр              | Значение                       |
|-----------------------|--------------------------------|
| position              | `{533.0, 4.0}`                 |
| alignment             | `TextDrawAlignment_Center` (2) |
| style                 | `TextDrawStyle_1` (1)          |
| letterSize            | `{0.4, 1.6}`                   |
| textSize              | `{400.0, 17.0}`                |
| letterColour          | `FFFFFFFF`                     |
| box                   | `false` (0)                    |
| boxColour             | `00000080`                     |
| backgroundColour      | `000000FF`                     |
| proportional          | `true` (1)                     |
| selectable            | `false` (0)                    |
| shadow / outline      | `0` / `0`                      |
| текст                 | `HARDWAY` (латиница)           |

preview-поля — дефолтные (модель 0, поворот 0/0/0, зум 1.0, цвета машины -1/-1).

Текст фиксированный, латиница (textdraw не рисуют кириллицу). `create` сам
санитизирует текст и клампит числовые параметры.

## Краевые случаи

- Компонент textdraw недоступен (`isAvailable()` == false) → логотип не
  создаётся, `m_logoId` остаётся -1, предупреждение в лог.
- Пул textdraw исчерпан (`create` вернул `nullptr`) → `m_logoId` остаётся -1,
  предупреждение в лог; `showForPlayer` не вызывается.
