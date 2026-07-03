#include "Services/ParkedVehicleService/ParkedVehicleService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <cmath>
#include <mysqlx/xdevapi.h>
#include <string>
#include <utility>

namespace
{
// Кламп персистентного fuel 0..CAP; мусор (NaN/Inf/отрицательное/сверх капасити).
float clampParkedFuel(double raw)
{
    if (!std::isfinite(raw))
    {
        return VehicleService::FUEL_CAPACITY; // мусор из БД — полный бак безопаснее пустого
    }
    if (raw < 0.0)
    {
        return 0.0f;
    }
    if (raw > static_cast<double>(VehicleService::FUEL_CAPACITY))
    {
        return VehicleService::FUEL_CAPACITY;
    }
    return static_cast<float>(raw);
}
} // namespace

void ParkedVehicleService::bind(VehicleService &vehicleService, FamilyService &familyService)
{
    m_vehicleService = &vehicleService;
    m_familyService = &familyService;
}

void ParkedVehicleService::loadParked(long long dbId, AccountId owner, int model, Vector3 spot, float angle,
                                      int familyId, float fuel)
{
    if (dbId < 0 || m_byDbId.count(dbId))
    {
        return; // мусорный/дублирующийся dbId — не грузим
    }
    Parked parked;
    parked.personalVehicleId = dbId;
    parked.ownerAccountId = owner;
    parked.model = model;
    parked.spot = spot;
    parked.angle = angle;
    parked.familyId = familyId;
    parked.fuel = clampParkedFuel(fuel);
    m_byDbId.emplace(dbId, parked);
    m_byAccount.emplace(owner, dbId);
    // NO_FAMILY (личная) в семейный индекс НЕ кладём — иначе parkedOfFamily(NO_FAMILY)
    // вернул бы все личные машины, а крайние случаи зацепили бы не то.
    if (familyId != FamilyService::NO_FAMILY)
    {
        m_byFamily.emplace(familyId, dbId);
    }
}

ParkedVehicleService::Result ParkedVehicleService::park(long long dbId, AccountId owner, int model, Vector3 spot,
                                                        float angle, float fuel)
{
    if (dbId < 0 || !m_vehicleService)
    {
        return Result::Invalid;
    }
    if (m_byDbId.count(dbId))
    {
        return Result::AlreadyParked;
    }

    const float clampedFuel = clampParkedFuel(fuel);
    loadParked(dbId, owner, model, spot, angle, FamilyService::NO_FAMILY, clampedFuel); // те же индексы, что и на загрузке

    // Write-through INSERT (family_id = NO_FAMILY: личная у дома). fuel — РЕАЛЬНЫЙ
    // снимок на момент парковки (машина ре-тегается НА МЕСТЕ, не пересоздаётся —
    // писать дефолт БД вместо наезженного остатка открыло бы бесплатную доливку).
    // Ошибка БД лишь логируется (память уже обновлена, экземпляр создаст система).
    // На следующем старте зеркало из БД либо подтвердит, либо снимет.
    DatabaseManager::throwQuery(
        [dbId, owner, model, spot, angle, clampedFuel](mysqlx::Schema schema)
        {
            schema.getTable("parked_vehicle")
                .insert("personal_vehicle_id", "owner_account_id", "model", "x", "y", "z", "angle", "family_id",
                       "fuel")
                .values(dbId, owner, model, spot.x, spot.y, spot.z, angle, FamilyService::NO_FAMILY,
                       static_cast<double>(clampedFuel))
                .execute();
        },
        [dbId](const std::string &error)
        { LogManager::log(Error, "ParkedVehicleService: park persist failed (dbId " + std::to_string(dbId) +
                                     "): " + error); });

    return Result::Ok;
}

void ParkedVehicleService::setFuel(long long dbId, float fuel)
{
    if (!std::isfinite(fuel))
    {
        return; // мусорный float не оседает в снимке
    }
    const auto it = m_byDbId.find(dbId);
    if (it == m_byDbId.end())
    {
        return;
    }
    it->second.fuel =
        fuel < 0.0f ? 0.0f : (fuel > VehicleService::FUEL_CAPACITY ? VehicleService::FUEL_CAPACITY : fuel);
}

ParkedVehicleService::Result ParkedVehicleService::unpark(long long dbId)
{
    const auto it = m_byDbId.find(dbId);
    if (it == m_byDbId.end())
    {
        return Result::NotParked;
    }

    // Копия — уничтожаем экземпляр и чистим индексы ДО destroy (destroyed-наблюдатель
    // увидит уже снятый vehicleId и сделает no-op — без ре-энтрантной порчи индексов).
    const Parked parked = it->second;
    eraseFromMemory(dbId);
    destroyInstance(parked);

    // Write-through DELETE строки парковки.
    DatabaseManager::throwQuery(
        [dbId](mysqlx::Schema schema)
        {
            schema.getTable("parked_vehicle")
                .remove()
                .where("personal_vehicle_id = :id")
                .bind("id", dbId)
                .execute();
        },
        [dbId](const std::string &error)
        { LogManager::log(Error, "ParkedVehicleService: unpark persist failed (dbId " + std::to_string(dbId) +
                                     "): " + error); });

    return Result::Ok;
}

ParkedVehicleService::Result ParkedVehicleService::unparkKeepInstance(long long dbId)
{
    const auto it = m_byDbId.find(dbId);
    if (it == m_byDbId.end())
    {
        return Result::NotParked;
    }

    // Снимаем ТОЛЬКО запись+обратные ссылки (eraseFromMemory чистит m_byVehicleId по
    // vehicleId), живой экземпляр в мире остаётся: снятие НА МЕСТЕ, его подхватил
    // PersonalVehicleService. destroy НЕ зовём — машина не исчезает и не пересоздаётся.
    eraseFromMemory(dbId);

    // Write-through DELETE строки парковки (право владения в personal_vehicle цело).
    DatabaseManager::throwQuery(
        [dbId](mysqlx::Schema schema)
        {
            schema.getTable("parked_vehicle")
                .remove()
                .where("personal_vehicle_id = :id")
                .bind("id", dbId)
                .execute();
        },
        [dbId](const std::string &error)
        { LogManager::log(Error, "ParkedVehicleService: unpark persist failed (dbId " + std::to_string(dbId) +
                                     "): " + error); });

    return Result::Ok;
}

ParkedVehicleService::Result ParkedVehicleService::shareToFamily(long long dbId, int familyId)
{
    if (familyId == FamilyService::NO_FAMILY)
    {
        return Result::Invalid;
    }
    const auto it = m_byDbId.find(dbId);
    if (it == m_byDbId.end())
    {
        return Result::NotParked;
    }
    if (it->second.familyId != FamilyService::NO_FAMILY)
    {
        return Result::AlreadyShared;
    }

    // Только смена режима доступа: экземпляр Owner::Parked не трогаем. Правим индекс
    // семьи (личная переходит в расшаренную — теперь есть ключ familyId).
    it->second.familyId = familyId;
    m_byFamily.emplace(familyId, dbId);
    persistFamily(dbId, familyId);
    // Личная -> семья: если была деспавнена (владелец оффлайн) — теперь desired=true,
    // наблюдатель заспавнит (семья должна её видеть). Владелец онлайн — экземпляр уже в
    // мире, приведение = no-op (идемпотентно).
    notifyReconcile(dbId);
    return Result::Ok;
}

ParkedVehicleService::Result ParkedVehicleService::unshareFromFamily(long long dbId)
{
    const auto it = m_byDbId.find(dbId);
    if (it == m_byDbId.end())
    {
        return Result::NotParked;
    }
    if (it->second.familyId == FamilyService::NO_FAMILY)
    {
        return Result::NotShared;
    }

    // Снять ключ семьи из индекса (расшаренная -> личная); экземпляр остаётся.
    const int oldFamilyId = it->second.familyId;
    it->second.familyId = FamilyService::NO_FAMILY;
    auto range = m_byFamily.equal_range(oldFamilyId);
    for (auto i = range.first; i != range.second; ++i)
    {
        if (i->second == dbId)
        {
            m_byFamily.erase(i);
            break;
        }
    }
    persistFamily(dbId, FamilyService::NO_FAMILY);
    // Семья -> личная: если владелец оффлайн — desired=false, наблюдатель деспавнит
    // (личная оффлайн-владельца в мире не живёт); онлайн — остаётся до его выхода.
    // onFamilyDissolved/onOwnerLeftFamily нотифицируют ТРАНЗИТИВНО через этот вызов.
    notifyReconcile(dbId);
    return Result::Ok;
}

ParkedVehicleService::Result ParkedVehicleService::respawnHome(long long dbId)
{
    const auto it = m_byDbId.find(dbId);
    if (it == m_byDbId.end())
    {
        return Result::NotParked;
    }
    const int vehicleId = it->second.vehicleId;
    if (vehicleId == -1)
    {
        return Result::NoInstance; // «в гараже» — экземпляра сейчас нет
    }
    if (!m_vehicleService)
    {
        return Result::Invalid;
    }
    IVehicle *vehicle = m_vehicleService->get(vehicleId);
    if (!vehicle)
    {
        return Result::NoInstance; // экземпляр пропал внешним destroy между кликами
    }
    if (m_vehicleService->getDriver(vehicleId) != -1)
    {
        return Result::Occupied; // за рулём водитель (владелец или член семьи) — не выдёргиваем
    }
    // Снимок ТЕКУЩЕГО остатка ДО respawn (тот сразу даёт полный бак) — то же поле
    // Parked::fuel, что источник правды деспавненной машины; персист в БД делает
    // ParkedVehicleSystem::onVehicleRespawned (переиспользует путь несанкционированной
    // смерти, см. заголовок метода).
    it->second.fuel = m_vehicleService->getFuel(vehicleId);
    m_vehicleService->respawn(*vehicle);
    return Result::Ok;
}

void ParkedVehicleService::setVehicleId(long long dbId, int vehicleId)
{
    const auto it = m_byDbId.find(dbId);
    if (it == m_byDbId.end())
    {
        return;
    }
    // Снять старую обратную ссылку, если была (пере-подача экземпляра).
    if (it->second.vehicleId >= 0)
    {
        m_byVehicleId.erase(it->second.vehicleId);
    }
    it->second.vehicleId = vehicleId;
    if (vehicleId >= 0)
    {
        m_byVehicleId[vehicleId] = dbId;
    }
}

void ParkedVehicleService::subscribeReconcile(ReconcileObserver observer)
{
    m_reconcileObservers.push_back(std::move(observer));
}

void ParkedVehicleService::onVehicleDestroyed(int vehicleId)
{
    const auto it = m_byVehicleId.find(vehicleId);
    if (it == m_byVehicleId.end())
    {
        return; // не наш экземпляр
    }
    const long long dbId = it->second;
    m_byVehicleId.erase(it);
    const auto parked = m_byDbId.find(dbId);
    if (parked != m_byDbId.end())
    {
        // Парковка остаётся (машина у дома, экземпляр пропал внешним destroy) —
        // обнуляем только id живого экземпляра. Штатное снятие — через unpark.
        parked->second.vehicleId = -1;
    }
}

bool ParkedVehicleService::isParked(long long dbId) const
{
    return dbId >= 0 && m_byDbId.count(dbId) != 0;
}

int ParkedVehicleService::parkedMode(long long dbId) const
{
    const auto it = m_byDbId.find(dbId);
    return it != m_byDbId.end() ? it->second.familyId : NOT_PARKED;
}

bool ParkedVehicleService::isAwayFromSpot(long long dbId) const
{
    const auto it = m_byDbId.find(dbId);
    if (it == m_byDbId.end() || it->second.vehicleId == -1 || !m_vehicleService)
    {
        return false;
    }
    IVehicle *vehicle = m_vehicleService->get(it->second.vehicleId);
    if (!vehicle)
    {
        return false;
    }
    // Близость горизонтальная (XY, без sqrt) — Z машины оседает на рельеф и дала
    // бы ложное «далеко» на своей же точке (как anyVehicleNear в VehicleService).
    const Vector3 pos = vehicle->getPosition();
    const float dx = pos.x - it->second.spot.x;
    const float dy = pos.y - it->second.spot.y;
    return dx * dx + dy * dy > HOME_SPOT_RADIUS * HOME_SPOT_RADIUS;
}

std::vector<long long> ParkedVehicleService::parkedByAccount(AccountId accountId) const
{
    std::vector<long long> result;
    const auto range = m_byAccount.equal_range(accountId);
    for (auto it = range.first; it != range.second; ++it)
    {
        result.push_back(it->second);
    }
    return result;
}

std::size_t ParkedVehicleService::countParkedByAccount(AccountId accountId) const
{
    // count даёт число записей с ключом — ровно припаркованные машины владельца
    // (личные + расшаренные, все в m_byAccount). Без аллокации, O(записей ключа).
    return m_byAccount.count(accountId);
}

std::vector<long long> ParkedVehicleService::parkedOfFamily(int familyId) const
{
    std::vector<long long> result;
    if (familyId == FamilyService::NO_FAMILY)
    {
        return result; // личные машины в m_byFamily не лежат — семьи с id NO_FAMILY нет
    }
    const auto range = m_byFamily.equal_range(familyId);
    for (auto it = range.first; it != range.second; ++it)
    {
        result.push_back(it->second);
    }
    return result;
}

const ParkedVehicleService::Parked *ParkedVehicleService::byDbId(long long dbId) const
{
    const auto it = m_byDbId.find(dbId);
    return it != m_byDbId.end() ? &it->second : nullptr;
}

const ParkedVehicleService::Parked *ParkedVehicleService::byVehicleId(int vehicleId) const
{
    const auto it = m_byVehicleId.find(vehicleId);
    if (it == m_byVehicleId.end())
    {
        return nullptr;
    }
    const auto parked = m_byDbId.find(it->second);
    return parked != m_byDbId.end() ? &parked->second : nullptr;
}

bool ParkedVehicleService::canDrive(int vehicleId, AccountId accountId) const
{
    const Parked *parked = byVehicleId(vehicleId);
    if (!parked)
    {
        return true; // не наша (не Parked) машина — гейт доступа не наш
    }
    if (parked->familyId == FamilyService::NO_FAMILY)
    {
        // Личная owner-only: только владелец. accountId серверный (из сессии); не
        // залогиненный (NO_ACCOUNT) не проходит даже если ownerAccountId битый.
        return accountId != PlayerSessionService::NO_ACCOUNT && accountId == parked->ownerAccountId;
    }
    if (!m_familyService)
    {
        return false; // расшаренная, сервис семей не связан — безопаснее не пускать
    }
    // Расшаренная: член семьи-получателя? familyByAccount работает и оффлайн-владельца
    // (m_accountFamily) — доступ не зависит от онлайна. O(1).
    return m_familyService->familyByAccount(accountId) == parked->familyId;
}

void ParkedVehicleService::onFamilyDissolved(int familyId)
{
    // Снять шеринг у всех машин семьи: экземпляры остаются припаркованы ЛИЧНО. N UPDATE
    // (машин мало, холодный путь) вместо batch-DELETE. parkedOfFamily снимок — берём
    // ДО unshareFromFamily (он правит m_byFamily под нами).
    const std::vector<long long> dbIds = parkedOfFamily(familyId);
    for (const long long dbId : dbIds)
    {
        unshareFromFamily(dbId);
    }
}

void ParkedVehicleService::onOwnerLeftFamily(AccountId accountId)
{
    // Снять шеринг только у РАСШАРЕННЫХ машин этого владельца (личные не трогать).
    const std::vector<long long> dbIds = parkedByAccount(accountId);
    for (const long long dbId : dbIds)
    {
        const auto it = m_byDbId.find(dbId);
        if (it != m_byDbId.end() && it->second.familyId != FamilyService::NO_FAMILY)
        {
            unshareFromFamily(dbId);
        }
    }
}

void ParkedVehicleService::destroyInstance(const Parked &parked)
{
    if (parked.vehicleId >= 0 && m_vehicleService)
    {
        m_vehicleService->destroy(parked.vehicleId);
    }
}

void ParkedVehicleService::eraseFromMemory(long long dbId)
{
    const auto it = m_byDbId.find(dbId);
    if (it == m_byDbId.end())
    {
        return;
    }
    if (it->second.vehicleId >= 0)
    {
        m_byVehicleId.erase(it->second.vehicleId);
    }
    // Снять из семейного индекса (только если расшарена — личная там не лежит).
    if (it->second.familyId != FamilyService::NO_FAMILY)
    {
        auto range = m_byFamily.equal_range(it->second.familyId);
        for (auto i = range.first; i != range.second; ++i)
        {
            if (i->second == dbId)
            {
                m_byFamily.erase(i);
                break;
            }
        }
    }
    // m_byAccount — ключ AccountId (int64).
    {
        auto range = m_byAccount.equal_range(it->second.ownerAccountId);
        for (auto i = range.first; i != range.second; ++i)
        {
            if (i->second == dbId)
            {
                m_byAccount.erase(i);
                break;
            }
        }
    }
    m_byDbId.erase(it);
}

void ParkedVehicleService::notifyReconcile(long long dbId)
{
    // Синхронный прогон в главном потоке (флип family_id зовётся из диалог-колбэка/
    // семейного события). Наблюдатель (система) читает свежую запись через byDbId.
    for (const ReconcileObserver &observer : m_reconcileObservers)
    {
        observer(dbId);
    }
}

void ParkedVehicleService::persistFamily(long long dbId, int familyId)
{
    // Write-through UPDATE family_id (share/unshare/крайние случаи). Ошибка лишь
    // логируется — память уже правильна, зеркало из БД подтвердит на следующем старте.
    DatabaseManager::throwQuery(
        [dbId, familyId](mysqlx::Schema schema)
        {
            schema.getTable("parked_vehicle")
                .update()
                .set("family_id", familyId)
                .where("personal_vehicle_id = :id")
                .bind("id", dbId)
                .execute();
        },
        [dbId](const std::string &error)
        { LogManager::log(Error, "ParkedVehicleService: family_id update failed (dbId " + std::to_string(dbId) +
                                     "): " + error); });
}
