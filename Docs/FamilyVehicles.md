# Семейные машины (устарело)

Фича обобщена в **«Припаркованные у дома машины»** — см.
[`Docs/ParkedVehicles.md`](ParkedVehicles.md).

«Семейная машина» больше не отдельная сущность: парковка у дома — базовое состояние
личной машины, шеринг семье — режим доступа поверх припаркованной машины. Код:
`Src/Services/ParkedVehicleService`, `Src/Systems/ParkedVehicleSystem`, тег
`VehicleService::Owner::Parked`, таблица `parked_vehicle`.
