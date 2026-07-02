#pragma once

// Серверный справочник ремзон SA — гейт клиентских SCM-событий (EnterExitModShop /
// AddComponent / SetColour / SetPaintjob): ядро open.mp пропускает их от любого
// водителя застримленной машины и НИКАКИХ зон не знает (vehicles_impl.hpp,
// PlayerSCMEventHandler), а легальные мод-шоп и Pay'n'Spray чинят машину на
// клиенте — сервер обязан признать рост HP (sanctionRepair). Санкция выдаётся
// ТОЛЬКО когда машина по СЕРВЕРНОЙ позиции (последний принятый синк) стоит у
// известной зоны — см. VehicleService::onModShop/validateMod/validatePaintJob/
// validateRespray.
//
// Мод-шоп физически переносит машину С ВОДИТЕЛЕМ в интерьер (universe-координаты,
// z~1000) — честный «телепорт» без серверного teleport(). onModShop на принятом
// enter/exit выдаёт точечный грейс позиции игрока
// (PlayerLocationService::grantModShopTeleportGrace), иначе PlayerLocationService
// принял бы въезд в шоп за телепорт-хак и выдернул бы честного тюнера обратно.
//
// Зоны статичны (карта SA). Политика таблицы — КОНСЕРВАТИВНАЯ: лишняя или
// сдвинутая запись — МОЛЧАЩАЯ дыра (пятачок «бесплатного ремонта», который сам
// себя в журнале не покажет), тогда как пропущенная/неточная реальная зона —
// ложный отказ, и он ГРОМКИЙ: честные перекраски лягут в журнал античита с
// координатами машины в detail — по ним запись добавляется/правится здесь.
namespace RepairZones
{
struct Zone
{
    float x, y, z;
};

// Радиус принятия: в момент SCM-события машина стоит в воротах/внутри гаража —
// метры от центра зоны; запас покрывает пролёт ворот, глубину гаража и
// неточность замера центра. Радиус консервативный (та же асимметрия, что у
// таблицы): недобор виден в журнале и чинится правкой, перебор — молчащая дыра.
inline constexpr float ACCEPT_RADIUS = 20.0f;

// Мод-шопы: 5 гаражей (3 TransFender, Loco Low Co, Wheel Arch Angels) плюс их
// 3 интерьера (universe-координаты, z ~1000): во время сессии клиент синкает
// интерьерную позицию машины — AddComponent/SetPaintjob/SetColour/exit приходят
// и оттуда.
inline constexpr Zone MOD_SHOPS[] = {
    {1041.3f, -1025.5f, 32.3f}, // TransFender, Temple (LS)
    {2644.9f, -2039.2f, 13.6f}, // Loco Low Co, Willowfield (LS)
    {-1936.0f, 240.0f, 34.5f},  // TransFender, Doherty (SF)
    {-2712.4f, 217.9f, 4.2f},   // Wheel Arch Angels, Ocean Flats (SF)
    {2386.9f, 1043.7f, 10.8f},  // TransFender, Come-A-Lot (LV)
    {617.0f, -8.1f, 1000.9f},   // интерьер TransFender (interior 1)
    {616.5f, -75.6f, 997.6f},   // интерьер Loco Low Co (interior 2)
    {616.2f, -124.9f, 997.6f},  // интерьер Wheel Arch Angels (interior 3)
};

// Pay'n'Spray: сюда приходит только SetColour (перекраска чинит машину на
// клиенте) — сессии enter/exit у покрасок нет. 10 публичных гаражей SA плюс
// Michelle's Auto Repair (тоже красит/чинит на клиенте).
inline constexpr Zone PAY_N_SPRAY[] = {
    {2064.6f, -1831.4f, 13.5f}, // Idlewood (LS)
    {2065.6f, -2260.0f, 13.5f}, // El Corona, у Unity Station (LS)
    {1025.1f, -1024.5f, 32.1f}, // Temple, рядом с TransFender (LS)
    {488.2f, -1735.0f, 11.1f},  // Santa Maria Beach (LS)
    {720.1f, -455.9f, 16.4f},   // Dillimore (Red County)
    {-1904.5f, 278.0f, 41.0f},  // Doherty (SF)
    {-2425.7f, 1021.8f, 50.1f}, // Juniper Hollow (SF)
    {-1787.3f, 1210.1f, 25.1f}, // Michelle's Auto Repair, Downtown (SF)
    {-1420.5f, 2584.2f, 55.6f}, // El Quebrados (Tierra Robada)
    {-100.1f, 1118.2f, 19.7f},  // Fort Carson (Bone County)
    {1975.1f, 2162.5f, 11.0f},  // Redsands East (LV)
};
} // namespace RepairZones
