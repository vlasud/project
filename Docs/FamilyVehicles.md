# Семейные машины (устарело)

Фича обобщена в **«Припаркованные у дома машины»** — см.
[`Docs/ParkedVehicles.md`](ParkedVehicles.md).

«Семейная машина» больше не отдельная сущность: парковка у дома — базовое состояние
личной машины, шеринг семье — режим доступа поверх припаркованной машины. Код:
`Src/Services/ParkedVehicleService`, `Src/Systems/ParkedVehicleSystem`, тег
`VehicleService::Owner::Parked`, таблица `parked_vehicle`.

Список `/family` → «Транспорт семьи» (код — `FamilySystem::showVehicles`) —
READ-ONLY TABLIST_HEADERS в ТОМ ЖЕ формате, что `/car` → «Мои машины» («Машина |
Где находится | Топливо»): обе точки используют один и тот же построитель строки
`ParkedVehicleRow::build` (`Src/Services/ParkedVehicleService/ParkedVehicleRow.h`),
чтобы текст не расходился между входами. «Забрать» доступа семьи из `/family`
УБРАНО — снять шеринг может ТОЛЬКО владелец машины через `/car` → «Вернуть от
семьи» (см. `Docs/ParkedVehicles.md`). Тексты — `Docs/GameDesign/UI_Texts.md`.
