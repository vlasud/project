#pragma once

#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"

struct IPlayer;

// Единый источник правды для двух попапов «двигатель не работает», общих для
// клавишного тумблера (VehicleControlSystem) и меню /car (CarMenuSystem): текст,
// цвет и длительность в ОДНОМ месте, чтобы обе точки показывали одно и то же и
// не разъезжались. Показ — через единый ScreenNoticeService (textdraw-попап, не
// чат и не native GameText — см. Docs/ScreenNotice.md). Тексты ASCII (латиница,
// cp1251-идентичны) — конвертация u() не нужна. Цвета: красный = тревога/ошибка
// (чинится только repair()), жёлтый = предупреждение (восстановимо refuel()) —
// свод цветов в Docs/GameDesign/UI_Texts.md.
namespace VehicleEngineNotice
{
// Общий срок показа обоих попапов: заметить успевает, баннером не залипает.
inline constexpr Milliseconds SHOW_TIME{3000};

// Поломка двигателя (HP добит до порога заглохания) — красный.
inline void showEngineBroken(ScreenNoticeService &notice, IPlayer &player)
{
    notice.show(player, "engine is broken", SHOW_TIME, Colour(0xE0, 0x30, 0x30, 0xFF));
}

// Пустой бак (двигатель заглох сам / не заводится) — жёлтый.
inline void showNoFuel(ScreenNoticeService &notice, IPlayer &player)
{
    notice.show(player, "no fuel", SHOW_TIME, Colour(0xF0, 0xC0, 0x30, 0xFF));
}
} // namespace VehicleEngineNotice
