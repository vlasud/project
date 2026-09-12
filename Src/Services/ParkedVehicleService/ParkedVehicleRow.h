#pragma once

#include "Services/Core/VehicleService/VehicleModelNames.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include <fmt/format.h>
#include <player.hpp>
#include <string>

// Единый источник строки «машина» для TABLIST-списков /car и /family: обе точки
// (CarMenuSystem::showMyCars, FamilySystem::showVehicles) показывают одну и ту же
// раскладку колонок «Машина | Где находится | Топливо» — текст не должен
// разъезжаться между входами. Header-only (аналог VehicleEngineNotice).
namespace ParkedVehicleRow
{
// Статус машины для списков (единый словарь). Логика размещения ОДИНАКОВА для
// личной и расшаренной — различается только слово точки: «у дома» (личная стоит
// у своей точки) / «у дома семьи» (расшаренная стоит у точки — все семейные
// машины стоят у дома лидера: шарит только лидер свою припаркованную, его дом и
// есть «дом семьи»); «брошена» — припаркованная (любая) оставлена дальше
// HOME_SPOT_RADIUS от точки; «в гараже» — припаркованная НЕ вызвана (живого
// экземпляра нет, у точки никто не ждёт); «на парковке» (не в мире) / «вызвана»
// (в мире, сессионная).
inline const char *placementStatus(const ParkedVehicleService &parked, long long dbId, int liveVehicleId)
{
    const int mode = parked.parkedMode(dbId);
    if (mode == ParkedVehicleService::NOT_PARKED)
        return liveVehicleId == -1 ? "на парковке" : "вызвана";
    if (liveVehicleId == -1)
        return "в гараже"; // припаркованная, но не вызванная — в мире её нет
    if (parked.isAwayFromSpot(dbId))
        return "брошена";
    return mode == FamilyService::NO_FAMILY ? "у дома" : "у дома семьи";
}

// Готовая строка TABLIST «{имя}\t{где находится}\t{остаток}/{CAP}». dbId — запись
// парковки (-1, если машина не припаркована — обычная сессионная); liveVehicleId —
// id живого экземпляра в мире (-1, если машины нет в мире), вычисляется
// вызывающим (у /car — через свой liveVehicleId() с учётом detached-владения, у
// /family — просто Parked::vehicleId, машина всегда припаркована по определению
// списка). players — пул для оверлея водителя («Ник[id]» вместо статуса, если за
// рулём кто-то сидит). model — модель для имени (/family — Parked::model, /car —
// OwnedVehicle::model). fallbackFuel — снимок топлива для случая «нет ни живого
// экземпляра, ни записи парковки» (сессионная «на парковке» — источник правды
// PersonalVehicleService::OwnedVehicle::fuel; у /family такого случая нет).
inline std::string build(const ParkedVehicleService &parked, const VehicleService &vehicles, IPlayerPool &players,
                         long long dbId, int liveVehicleId, int model,
                         float fallbackFuel = VehicleService::FUEL_CAPACITY)
{
    const int driverId = liveVehicleId != -1 ? vehicles.getDriver(liveVehicleId) : -1;
    IPlayer *driver = driverId != -1 ? players.get(driverId) : nullptr;
    const std::string where = driver ? fmt::format("{}[{}]", driver->getName().to_string(), driverId)
                                     : std::string(placementStatus(parked, dbId, liveVehicleId));

    // Топливо: живой экземпляр -> getFuel (источник правды в мире); иначе —
    // персистентный снимок записи парковки; иначе (нет ни того, ни другого) —
    // fallbackFuel вызывающей стороны.
    const ParkedVehicleService::Parked *rec = dbId != -1 ? parked.byDbId(dbId) : nullptr;
    const float fuel = liveVehicleId != -1 ? vehicles.getFuel(liveVehicleId) : (rec ? rec->fuel : fallbackFuel);

    return fmt::format("{}\t{}\t{}/{}", VehicleModelNames::displayName(model), where, static_cast<int>(fuel),
                       static_cast<int>(VehicleService::FUEL_CAPACITY));
}
} // namespace ParkedVehicleRow
