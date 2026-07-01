#include "Systems/ParkedVehicleSystem/ParkedVehicleSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Utils/Encoding/Encoding.h"
#include <cstdint>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <tuple>
#include <vector>

namespace
{
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
    // (машина не уничтожается) — vehicleId сохраняется, что и нужно.
    m_vehicleService.subscribeDestroyed(
        [this](IVehicle &vehicle) { m_parkedService.onVehicleDestroyed(vehicle.getID()); });

    // Гейт «за руль»: чужой не садится в припаркованную машину.
    m_vehicleService.subscribeDriverGate([this](IPlayer &player, IVehicle &vehicle)
                                         { return onDriverGate(player, vehicle); });

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

void ParkedVehicleSystem::loadParked()
{
    // Кортеж: personal_vehicle_id, owner_account_id, model, x, y, z, angle, family_id.
    using ParkedRow =
        std::tuple<std::int64_t, ParkedVehicleService::AccountId, int, double, double, double, double, int>;
    DatabaseManager::selectQuery<std::vector<ParkedRow>>(
        [](mysqlx::Schema schema)
        {
            mysqlx::RowResult rows = schema.getTable("parked_vehicle")
                                         .select("personal_vehicle_id", "owner_account_id", "model", "x", "y", "z",
                                                 "angle", "family_id")
                                         .execute();
            std::vector<ParkedRow> result;
            while (mysqlx::Row row = rows.fetchOne())
            {
                result.emplace_back(row.get(0).get<std::int64_t>(),
                                    row.get(1).get<ParkedVehicleService::AccountId>(), row.get(2).get<int>(),
                                    row.get(3).get<double>(), row.get(4).get<double>(), row.get(5).get<double>(),
                                    row.get(6).get<double>(), row.get(7).get<int>());
            }
            return result;
        },
        [this](std::vector<ParkedRow> parked)
        {
            for (const auto &[dbId, owner, model, x, y, z, angle, familyId] : parked)
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
                m_parkedService.loadParked(dbId, owner, model, spot, ang, effectiveFamilyId);
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
    IVehicle *veh = m_vehicleService.create(rec->model, rec->spot, rec->angle, -1, -1,
                                            VehicleService::Owner::Parked, -1);
    if (veh)
    {
        m_parkedService.setVehicleId(dbId, veh->getID());
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
