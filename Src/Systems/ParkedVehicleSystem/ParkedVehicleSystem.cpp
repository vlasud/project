#include "Systems/ParkedVehicleSystem/ParkedVehicleSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/VehicleService/VehicleModelNames.h"
#include "Utils/Encoding/Encoding.h"
#include <cstdint>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <tuple>
#include <vector>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

ParkedVehicleSystem::ParkedVehicleSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_parkedService(serviceRegister.getService<ParkedVehicleService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_familyService(serviceRegister.getService<FamilyService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    // Привязка зависимостей сервиса (реестр создаёт его дефолтным ctor).
    m_parkedService.bind(m_vehicleService, m_familyService);

    // Экземпляр припаркованной машины уничтожен (unpark/park/внешний destroy) —
    // обнулить vehicleId живого экземпляра. На death-респавне destroyed НЕ приходит
    // (машина не уничтожается) — vehicleId сохраняется, что и нужно. Заодно снимаем
    // висящий снимок m_pendingFuelRestore (если был): vehicleId — переиспользуемый
    // pool-слот, и без очистки следующая СОВСЕМ ДРУГАЯ машина на этом id ложно
    // унаследовала бы чужой fuel на своём первом респавне.
    m_vehicleService.subscribeDestroyed(
        [this](IVehicle &vehicle)
        {
            m_parkedService.onVehicleDestroyed(vehicle.getID());
            m_pendingFuelRestore.erase(vehicle.getID());
        });

    // Гейт «за руль»: чужой не садится в припаркованную машину.
    m_vehicleService.subscribeDriverGate([this](IPlayer &player, IVehicle &vehicle)
                                         { return onDriverGate(player, vehicle); });

    // Несанкционированная смерть (контракт смерти): только ФАКТ, без пула машин —
    // подписчик лишь читает запись Parked и шлёт сообщение владельцу МАШИНЫ (см.
    // контракт подписки в VehicleService.h). Снимает остаток топлива ДО возврата
    // машины (машина ещё валидна) — восстановит subscribeRespawned.
    m_vehicleService.subscribeUnsanctionedDeath([this](IVehicle &vehicle, bool returnsInPlace)
                                                { onUnsanctionedDeath(vehicle, returnsInPlace); });

    // Респавн (после несанкционированной смерти либо ядрового death-таймера):
    // VehicleService уже дал ПОЛНЫЙ бак по умолчанию — если для этого vehicleId есть
    // снимок (onUnsanctionedDeath), восстанавливаем его поверх дефолта.
    m_vehicleService.subscribeRespawned([this](IVehicle &vehicle) { onVehicleRespawned(vehicle); });

    // Жизненный цикл ЛИЧНОГО экземпляра — по сессии владельца (не raw disconnect).
    // Вход: его личные припаркованные появляются в мире на своих точках; расшаренные
    // уже стоят (спавн на старте/переходе). Выход: личные исчезают (запись+БД целы ->
    // респавн на следующем входе); расшаренные НЕ трогаем. Оба обработчика идемпотентны.
    m_sessionService.subscribeStart(
        [this](IPlayer &, const PlayerSessionService::Session &session) { onOwnerOnline(session.accountId); });
    m_sessionService.subscribeEnd(
        [this](IPlayer &, const PlayerSessionService::Session &session) { onOwnerOffline(session.accountId); });

    // Переход режима доступа (share/unshare/крайние случаи флипают family_id внутри
    // сервиса) -> привести экземпляр к желаемому состоянию. Сервис нотифицирует, система
    // драйвит create/destroy (ей доступны онлайн владельца и VehicleService).
    m_parkedService.subscribeReconcile([this](long long dbId) { reconcile(dbId); });
}

void ParkedVehicleSystem::initialize(IComponentList * /*components*/)
{
    // parked_vehicle грузим СТРОГО после семей: обе загрузки async, порядок даёт
    // только one-shot subscribeLoaded (семьи нужны для гейта РАСШАРЕННЫХ; личные семей
    // не требуют). Поздняя подписка после загрузки — колбэк сразу.
    m_familyService.subscribeLoaded([this]() { loadParked(); });
}

bool ParkedVehicleSystem::onDriverGate(IPlayer &player, IVehicle &vehicle)
{
    const int vehicleId = vehicle.getID();
    // Не-Parked машина -> canDrive вернёт true (гейт не наш) без обращения к сессии.
    // Parked-машина: сверяем по СЕРВЕРНОМУ accountId сессии (клиенту не верим).
    const ParkedVehicleService::AccountId accountId = m_sessionService.getAccountId(player.getID());
    if (m_parkedService.canDrive(vehicleId, accountId))
    {
        return true;
    }

    // Отказ — текст различает личную (замок владельца) и семейную (только члены).
    const ParkedVehicleService::Parked *parked = m_parkedService.byVehicleId(vehicleId);
    if (parked && parked->familyId == FamilyService::NO_FAMILY)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Машина заперта владельцем"));
    }
    else
    {
        player.sendClientMessage(ERROR_COLOUR, u("За руль этой машины пускают только членов семьи"));
    }
    return false;
}

void ParkedVehicleSystem::onUnsanctionedDeath(IVehicle &vehicle, bool returnsInPlace)
{
    const int vehicleId = vehicle.getID();
    // Не-Parked машина (её ведёт другая система/личная сессионная) — не наша.
    const ParkedVehicleService::Parked *parked = m_parkedService.byVehicleId(vehicleId);
    if (!parked)
        return;

    // Снимок остатка топлива ДО возврата — НЕЗАВИСИМО от того, онлайн ли владелец
    // (топливо не должно теряться из-за того, что уведомлять некого). Машина ещё
    // валидна (died-lock диспатча смерти держит её живой); сам возврат случится
    // позже (respawnIfDead ~100 мс либо ядровой death-таймер ~10 с).
    // subscribeRespawned применит снимок поверх дефолтного полного бака: закрывает
    // «бесплатный эвакуатор с заправкой» (Docs/GameDesign/Economy.md). НЕЛЬЗЯ трогать
    // пул машин здесь (контракт subscribeUnsanctionedDeath) — только чтение+запись
    // локальной мапы, что и делаем.
    m_pendingFuelRestore[vehicleId] = m_vehicleService.getFuel(vehicleId);

    // Владелец МАШИНЫ (не члены семьи — уведомляем только его, см. UI_Texts.md):
    // резолв через PlayerSessionService::playerByAccount (единственный источник
    // правды об онлайне), НЕ по тегу VehicleService (ownerId тега служебный/-1).
    const int ownerId = m_sessionService.playerByAccount(parked->ownerAccountId);
    if (ownerId < 0)
        return; // владелец оффлайн — уведомление молча пропускаем (контракт: только online)
    IPlayer *owner = m_core.getPlayers().get(ownerId);
    if (!owner)
        return; // страховка от гонки колбэка/дисконнекта в тот же тик

    const std::string name = VehicleModelNames::displayName(vehicle.getModel());
    if (returnsInPlace)
    {
        owner->sendClientMessage(INFO_COLOUR,
                                 u(fmt::format("Вашу машину {} пытались уничтожить, но она цела", name)));
    }
    else
    {
        owner->sendClientMessage(
            INFO_COLOUR,
            u(fmt::format("Вашу машину {} пытались уничтожить. Она эвакуирована к вашему дому", name)));
    }
}

void ParkedVehicleSystem::onVehicleRespawned(IVehicle &vehicle)
{
    const int vehicleId = vehicle.getID();
    const auto it = m_pendingFuelRestore.find(vehicleId);
    if (it == m_pendingFuelRestore.end())
        return; // не наш снимок (обычный респавн/чужая запись) — полный бак остаётся
    const float fuel = it->second;
    m_pendingFuelRestore.erase(it);
    // Запись Parked могла исчезнуть между смертью и возвратом (unpark в редком окне) —
    // тогда снимок применить некуда, оставляем дефолтный полный бак от VehicleService.
    const ParkedVehicleService::Parked *parked = m_parkedService.byVehicleId(vehicleId);
    if (!parked)
        return;
    m_parkedService.setFuel(parked->personalVehicleId, fuel); // снимок в память записи
    m_vehicleService.setFuel(vehicle, fuel);                  // и в живой экземпляр
    persistFuel(parked->personalVehicleId, fuel);
}

void ParkedVehicleSystem::persistFuel(long long dbId, float fuel)
{
    DatabaseManager::throwQuery(
        [dbId, fuel](mysqlx::Schema schema)
        {
            schema.getTable("parked_vehicle")
                .update()
                .set("fuel", static_cast<double>(fuel))
                .where("personal_vehicle_id = :id")
                .bind("id", dbId)
                .execute();
        },
        [dbId](const std::string &error)
        {
            LogManager::log(Error,
                            "ParkedVehicleSystem: failed to persist fuel (dbId " + std::to_string(dbId) +
                                "): " + error);
        });
}

void ParkedVehicleSystem::loadParked()
{
    // Кортеж: personal_vehicle_id, owner_account_id, model, x, y, z, angle, family_id, fuel.
    using ParkedRow = std::tuple<std::int64_t, ParkedVehicleService::AccountId, int, double, double, double, double,
                                int, double>;
    DatabaseManager::selectQuery<std::vector<ParkedRow>>(
        [](mysqlx::Schema schema)
        {
            mysqlx::RowResult rows = schema.getTable("parked_vehicle")
                                         .select("personal_vehicle_id", "owner_account_id", "model", "x", "y", "z",
                                                 "angle", "family_id", "fuel")
                                         .execute();
            std::vector<ParkedRow> result;
            while (mysqlx::Row row = rows.fetchOne())
            {
                result.emplace_back(row.get(0).get<std::int64_t>(),
                                    row.get(1).get<ParkedVehicleService::AccountId>(), row.get(2).get<int>(),
                                    row.get(3).get<double>(), row.get(4).get<double>(), row.get(5).get<double>(),
                                    row.get(6).get<double>(), row.get(7).get<int>(), row.get(8).get<double>());
            }
            return result;
        },
        [this](std::vector<ParkedRow> parked)
        {
            for (const auto &[dbId, owner, model, x, y, z, angle, familyId, fuel] : parked)
            {
                // Расшаренная с несуществующей семьёй -> деградация в личную (машина
                // ценна сама по себе как припаркованная; не теряем). family_id
                // NO_FAMILY уже трактуется как личная — семью для него не ищем.
                int effectiveFamilyId = familyId;
                if (familyId != FamilyService::NO_FAMILY && !m_familyService.getFamily(familyId))
                {
                    LogManager::log(Warning,
                                    fmt::format("ParkedVehicleSystem: parked {} shared to unknown family {}, "
                                                "degraded to personal",
                                                dbId, familyId));
                    effectiveFamilyId = FamilyService::NO_FAMILY;
                }
                const Vector3 spot{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
                const float ang = static_cast<float>(angle);
                // fuel клампится в сервисе (мусор из БД — NaN/отрицательное/сверх капасити).
                m_parkedService.loadParked(dbId, owner, model, spot, ang, effectiveFamilyId,
                                          static_cast<float>(fuel));
                // Приводим экземпляр к инвариату «в мире <=> семья ИЛИ владелец онлайн»
                // через reconcile: расшаренная заводится всегда; личная — только если
                // владелец УЖЕ онлайн (редкое стартовое окно, когда игрок вошёл ДО прихода
                // async-загрузки parked; иначе его личная стояла бы «в гараже» до перезахода).
                // Личная оффлайн-владельца остаётся деспавненной (заведётся на его входе).
                // spawnInstance идемпотентен — двойного спавна семейных нет.
                reconcile(dbId);
            }
        },
        [](const std::string &error)
        { LogManager::log(Error, "ParkedVehicleSystem: failed to load parked vehicles: " + error); });
}

bool ParkedVehicleSystem::ownerOnline(ParkedVehicleService::AccountId accountId) const
{
    // playerByAccount — единственный источник правды об онлайне (обратный индекс сессий);
    // свой set<AccountId> не ведём (риск рассинхрона на двойном входе/перелогине, которые
    // сервис уже разруливает). NO_ACCOUNT отсекаем до резолва (битая запись owner=0).
    return accountId != PlayerSessionService::NO_ACCOUNT && m_sessionService.playerByAccount(accountId) >= 0;
}

void ParkedVehicleSystem::spawnInstance(long long dbId)
{
    const ParkedVehicleService::Parked *rec = m_parkedService.byDbId(dbId);
    if (!rec || rec->vehicleId != -1)
    {
        return; // нет записи / уже в мире — идемпотентный no-op
    }
    // Ровно как стартовый спавн: spawn-позиция = spot (death-респавн ядра вернёт сюда),
    // цвета случайные (-1,-1; цвет не персистится), ownerId тега служебный -1 (гейт —
    // canDrive по записи, не по тегу). setVehicleId — синхронно за create (окна нет).
    const float fuel = rec->fuel; // снимок ДО create (rec — указатель в map, create его не инвалидирует)
    IVehicle *veh = m_vehicleService.create(rec->model, rec->spot, rec->angle, -1, -1,
                                            VehicleService::Owner::Parked, -1);
    if (veh)
    {
        m_parkedService.setVehicleId(dbId, veh->getID());
        // Восстановить ПЕРСИСТЕНТНЫЙ остаток поверх дефолтного полного бака от create
        // (закрывает «убрать оффлайн -> вернуться -> бесплатный полный бак»).
        m_vehicleService.setFuel(*veh, fuel);
    }
    else
    {
        LogManager::log(Error,
                        fmt::format("ParkedVehicleSystem: vehicle pool full, parked {} not spawned", dbId));
    }
}

void ParkedVehicleSystem::despawnInstance(long long dbId)
{
    const ParkedVehicleService::Parked *rec = m_parkedService.byDbId(dbId);
    if (!rec || rec->vehicleId == -1)
    {
        return; // нет записи / уже деспавнена — no-op
    }
    const int vehicleId = rec->vehicleId;
    // Снять остаток топлива ДО destroy (после уничтожения getFuel вернёт 0) — снимок
    // в память + write-through UPDATE (persistFuel), иначе следующий вход владельца
    // (spawnInstance) вернул бы машину с дефолтным полным баком.
    const float fuel = m_vehicleService.getFuel(vehicleId);
    m_parkedService.setFuel(dbId, fuel);
    persistFuel(dbId, fuel);
    // ПОРЯДОК: сперва снять обратную ссылку в записи (setVehicleId(-1)), ПОТОМ destroy.
    // Тогда синхронный destroyed-наблюдатель (onVehicleDestroyed по vehicleId) записи уже
    // не найдёт (m_byVehicleId снят) -> no-op, ре-энтрантной порчи индексов нет. Запись и
    // БД-строка НЕ трогаются — это НЕ unpark. Тот же паттерн, что unpark/reset (индексы
    // ДО destroy).
    m_parkedService.setVehicleId(dbId, -1);
    m_vehicleService.destroy(vehicleId);
}

void ParkedVehicleSystem::reconcile(long long dbId)
{
    const ParkedVehicleService::Parked *rec = m_parkedService.byDbId(dbId);
    if (!rec)
    {
        return;
    }
    // Желаемое: расшаренная семье живёт всегда; личная — только пока владелец онлайн.
    const bool desired = rec->familyId != FamilyService::NO_FAMILY || ownerOnline(rec->ownerAccountId);
    if (desired && rec->vehicleId == -1)
    {
        spawnInstance(dbId);
    }
    else if (!desired && rec->vehicleId != -1)
    {
        despawnInstance(dbId);
    }
}

void ParkedVehicleSystem::onOwnerOnline(ParkedVehicleService::AccountId accountId)
{
    // На subscribeStart аккаунт уже в m_online (сервис ставит его ДО прогона обсерверов),
    // поэтому ownerOnline(accountId) уже true. Но фильтруем по NO_FAMILY и зовём спавн
    // напрямую (расшаренные уже в мире, их не трогаем). parkedByAccount — копия vector,
    // перебор не инвалидируется правками индексов.
    for (const long long dbId : m_parkedService.parkedByAccount(accountId))
    {
        const ParkedVehicleService::Parked *rec = m_parkedService.byDbId(dbId);
        if (rec && rec->familyId == FamilyService::NO_FAMILY)
        {
            spawnInstance(dbId); // личная владельца -> в мир (идемпотентно)
        }
    }
}

void ParkedVehicleSystem::onOwnerOffline(ParkedVehicleService::AccountId accountId)
{
    // КРИТИЧНО: на subscribeEnd аккаунт ещё в m_online (сервис делает erase ПОСЛЕ прогона
    // end-обсерверов), значит ownerOnline(accountId) вернул бы true и reconcile НЕ деспавнил
    // бы личную. Поэтому деспавним ЯВНО (не через reconcile). Расшаренные (family_id !=
    // NO_FAMILY) не трогаем — они живут независимо от онлайна владельца.
    for (const long long dbId : m_parkedService.parkedByAccount(accountId))
    {
        const ParkedVehicleService::Parked *rec = m_parkedService.byDbId(dbId);
        if (rec && rec->familyId == FamilyService::NO_FAMILY)
        {
            despawnInstance(dbId); // личная владельца -> из мира, запись и БД целы
        }
    }
}
