#include "Systems/PersonalVehicleSystem/PersonalVehicleSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/VehicleService/VehicleModelNames.h"
#include "Utils/Encoding/Encoding.h"
#include <cstdint>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <utility>
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

PersonalVehicleSystem::PersonalVehicleSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_personalService(serviceRegister.getService<PersonalVehicleService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    // Привязка зависимости сервиса (реестр создаёт его дефолтным ctor).
    m_personalService.bind(m_vehicleService);

    // Жизненный цикл владения — по сессии (account-data, по конвенции; не raw
    // disconnect). Старт: грузим модели+fuel аккаунта из БД. Конец: СНАЧАЛА снять
    // остаток топлива живых экземпляров (персист), ПОТОМ reset (уничтожить машины +
    // очистить ПАМЯТЬ владения; право владения и fuel остаются в БД).
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session) { loadOwnership(player, session); });
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            const int playerId = player.getID();
            if (playerId < 0 || playerId >= MAX_PLAYERS)
                return;
            captureFuelBeforeReset(playerId); // fuel живых экземпляров -> память + БД
            m_personalService.reset(playerId);
        });

    // Машину могли уничтожить извне (взрыв, другая система, респаун-цикл, /destroyveh):
    // обнуляем её id во владении (владение остаётся, машину спавнят заново через
    // парковку) — нет висячих id и двойного destroy. Заодно снимаем висящий снимок
    // m_pendingFuelRestore (если был): vehicleId — переиспользуемый pool-слот, и без
    // очистки следующая СОВСЕМ ДРУГАЯ машина на этом id ложно унаследовала бы чужой
    // fuel на своём первом респавне.
    m_vehicleService.subscribeDestroyed(
        [this](IVehicle &vehicle)
        {
            m_personalService.onWorldVehicleDestroyed(vehicle.getID());
            m_pendingFuelRestore.erase(vehicle.getID());
        });

    // Смерть машины: died-наблюдатели получают ТОЛЬКО серверно-САНКЦИОНИРОВАННУЮ
    // смерть (explode()); несанкционированную (клиентская детонация пустой /
    // фейковый репорт) VehicleService гасит сам — машина возвращается целой
    // респавном, сюда не доходит.
    // Санкционированно умершую личную удаляем, чтобы она ПРОПАДАЛА, а не висела
    // мёртвым вреком и НЕ вернулась death-респавном ядра (game.vehicle_respawn_time;
    // respawnDelay=-1 гасит лишь респавн по простою) — немедленный destroy убирает
    // её до этого таймера, в том же тике.
    m_vehicleService.subscribeDied([this](IVehicle &vehicle) { onVehicleDied(vehicle); });

    // Несанкционированная смерть (контракт смерти): только ФАКТ, без пула машин —
    // подписчик лишь читает стейт и шлёт сообщение (см. контракт подписки в
    // VehicleService.h). Owner-фильтр — внутри onUnsanctionedDeath. Снимает остаток
    // топлива ДО возврата машины (машина ещё валидна) — восстановит subscribeRespawned.
    m_vehicleService.subscribeUnsanctionedDeath([this](IVehicle &vehicle, bool returnsInPlace)
                                                { onUnsanctionedDeath(vehicle, returnsInPlace); });

    // Респавн (после несанкционированной смерти либо ядрового death-таймера):
    // VehicleService уже дал ПОЛНЫЙ бак по умолчанию — если для этого vehicleId есть
    // снимок (onUnsanctionedDeath), восстанавливаем его поверх дефолта.
    m_vehicleService.subscribeRespawned([this](IVehicle &vehicle) { onVehicleRespawned(vehicle); });

    auto &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add("pvbuy", {{PlayerCommandService::Param::Int, "id модели"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 { buyDebug(player, args.getInt(0)); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL),
                 "дебаг: купить личную машину по id модели", PlayerCommandService::HelpCategory::Hidden);
}

void PersonalVehicleSystem::loadOwnership(IPlayer &player, const PlayerSessionService::Session &session)
{
    // SELECT (id, model, fuel) владения аккаунта; запрос И вычитка — на воркере
    // (возвращаем владеющий vector<tuple>, mysqlx-объект границу потока не
    // пересекает). dbId (id строки) нужен шерингу — по нему он ссылается на
    // конкретную машину. fuel — персистентный остаток бака (клампится в сервисе).
    using OwnershipRow = std::tuple<long long, int, double>;
    DatabaseManager::selectQuery<std::vector<OwnershipRow>>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult result = schema.getTable("personal_vehicle")
                                           .select("id", "model", "fuel")
                                           .where("account_id = :account")
                                           .bind("account", accountId)
                                           .execute();
            std::vector<OwnershipRow> rows;
            for (mysqlx::Row row = result.fetchOne(); row; row = result.fetchOne())
            {
                rows.emplace_back(row.get(0).get<std::int64_t>(), row.get(1).get<int>(), row.get(2).get<double>());
            }
            return rows;
        },
        [this, playerId = player.getID(), serial = session.serial](std::vector<OwnershipRow> rows)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/другая сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            // Живой игрок (мог выйти, пока шёл async-select).
            if (!m_core.getPlayers().get(playerId))
                return;
            // Машину НЕ спавним — игрок берёт её на парковке. load снимает гейт
            // покупки (loaded=true).
            m_personalService.load(playerId, rows);
        },
        [](const std::string &error)
        {
            // m_loaded НАМЕРЕННО остаётся false при сбое загрузки: число строк владения
            // в БД неизвестно, поэтому покупка обязана остаться заблокированной (buy ->
            // NotLoaded) — иначе buy с count=0 обошёл бы лимит. Доступ к машинам вернётся
            // на следующем логине. НЕ «чинить» под House-паттерн (loaded=true на ошибке) —
            // это открыло бы обход лимита при неизвестном числе строк.
            LogManager::log(Error, "PersonalVehicleSystem: failed to load ownership: " + error);
        });
}

void PersonalVehicleSystem::captureFuelBeforeReset(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    // Снимаем fuel КАЖДОЙ заспавненной записи, пока экземпляры ещё живы (reset
    // уничтожит их следом). Индекс стабилен — count() не меняется этим циклом.
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    for (int i = 0; i < static_cast<int>(owned.size()); ++i)
    {
        const int vehicleId = owned[i].vehicleId;
        if (vehicleId == -1)
            continue; // не заспавнена — снимать нечего, в памяти уже актуальный снимок
        const float fuel = m_vehicleService.getFuel(vehicleId);
        m_personalService.setFuel(playerId, i, fuel);
        persistFuel(m_personalService.dbIdOf(playerId, i), fuel);
    }
}

void PersonalVehicleSystem::persistFuel(long long dbId, float fuel)
{
    if (dbId < 0)
        return; // id ещё не присвоен (окно между покупкой и LAST_INSERT_ID) — снимок только в памяти
    DatabaseManager::throwQuery(
        [dbId, fuel](mysqlx::Schema schema)
        {
            schema.getTable("personal_vehicle")
                .update()
                .set("fuel", static_cast<double>(fuel))
                .where("id = :id")
                .bind("id", dbId)
                .execute();
        },
        [dbId](const std::string &error)
        {
            LogManager::log(Error,
                            "PersonalVehicleSystem: failed to persist fuel (dbId " + std::to_string(dbId) +
                                "): " + error);
        });
}

void PersonalVehicleSystem::onVehicleDied(IVehicle &vehicle)
{
    const int vehicleId = vehicle.getID();
    // Только ЛИЧНАЯ машина пропадает при смерти; чужие owner-теги (Faction/Work)
    // решают свою политику смерти сами. getOwner — серверный тег, O(1).
    if (m_vehicleService.getOwner(vehicleId) != VehicleService::Owner::Player)
        return;
    // Снять остаток топлива ДО destroy (после уничтожения getFuel вернёт 0 для
    // несуществующей машины) — persistFuel по dbId записи владения. ownerId тега —
    // playerId (сессионный ключ), поиск ownedIndex по vehicleId — линейный (мало).
    const int ownerId = m_vehicleService.getOwnerId(vehicleId);
    if (ownerId >= 0)
    {
        const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(ownerId);
        for (int i = 0; i < static_cast<int>(owned.size()); ++i)
        {
            if (owned[i].vehicleId == vehicleId)
            {
                const float fuel = m_vehicleService.getFuel(vehicleId);
                m_personalService.setFuel(ownerId, i, fuel);
                persistFuel(owned[i].dbId, fuel);
                break;
            }
        }
    }
    // destroy зовётся из died-наблюдателя внутри события смерти — машина залочена в
    // пуле, release откладывается до unlock (SDK MarkedPoolStorage): диспатч смерти
    // не рвётся, onVehicleDestroyed придёт позже и обнулит vehicleId владения
    // (владение СОХРАНЯЕТСЯ) — спавн заново только вручную на парковке.
    m_vehicleService.destroy(vehicleId);
}

void PersonalVehicleSystem::onUnsanctionedDeath(IVehicle &vehicle, bool returnsInPlace)
{
    const int vehicleId = vehicle.getID();
    // Только ЛИЧНАЯ (Owner::Player, сессионная — «в общем гараже»/вызванная с
    // парковки) машина: припаркованные у дома (Owner::Parked) — своя система
    // (ParkedVehicleSystem). getOwner — серверный тег, O(1).
    if (m_vehicleService.getOwner(vehicleId) != VehicleService::Owner::Player)
        return;

    // Снимок остатка топлива ДО возврата (машина ещё валидна — died-lock диспатча
    // смерти держит её живой; сам возврат случится позже — respawnIfDead ~100 мс
    // либо ядровой death-таймер ~10 с). subscribeRespawned применит его поверх
    // дефолтного полного бака: закрывает «бесплатный эвакуатор с заправкой»
    // (Docs/GameDesign/Economy.md «Задел на будущий сток»). НЕЛЬЗЯ трогать пул
    // машин здесь (контракт subscribeUnsanctionedDeath) — только чтение+запись
    // локальной мапы, что и делаем.
    m_pendingFuelRestore[vehicleId] = m_vehicleService.getFuel(vehicleId);

    // ownerId тега Player — playerId (сессионный ключ владельца), НЕ accountId.
    // Владение сессионное — машина существует только пока владелец онлайн (reset
    // на конце сессии уничтожает все её экземпляры), поэтому валидный playerId тега
    // уже означает «этот же игрок online»; проверка живого игрока — страховка от
    // гонки колбэка/дисконнекта в тот же тик, не смена владельца.
    const int ownerId = m_vehicleService.getOwnerId(vehicleId);
    IPlayer *owner = ownerId >= 0 ? m_core.getPlayers().get(ownerId) : nullptr;
    if (!owner)
        return; // владелец оффлайн — уведомление молча пропускаем (контракт: только online)

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
            u(fmt::format("Вашу машину {} пытались уничтожить. Она эвакуирована на парковку", name)));
    }
}

void PersonalVehicleSystem::onVehicleRespawned(IVehicle &vehicle)
{
    const int vehicleId = vehicle.getID();
    const auto it = m_pendingFuelRestore.find(vehicleId);
    if (it == m_pendingFuelRestore.end())
        return; // не наш снимок (обычный респавн/чужой owner-тег) — полный бак остаётся
    const float fuel = it->second;
    m_pendingFuelRestore.erase(it);
    // Owner-тег на респавне не меняется (VehicleService сохраняет владельца) — но
    // перепроверяем на случай, если машину успели ре-тегнуть в парковку между
    // смертью и возвратом (крайне маловероятно, т.к. припаркованная не детонирует
    // этим путём, но дешёвая страховка). Не-Player — оставляем дефолтный полный бак.
    if (m_vehicleService.getOwner(vehicleId) != VehicleService::Owner::Player)
        return;
    m_vehicleService.setFuel(vehicle, fuel);
}

void PersonalVehicleSystem::buyDebug(IPlayer &player, int model)
{
    const int playerId = player.getID();

    // accountId — серверный (из сессии), не от клиента: владение пишется в БД по нему.
    const PlayerSessionService::AccountId accountId = m_sessionService.getAccountId(playerId);
    if (accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сначала войдите в аккаунт"));
        return;
    }

    // Лимит, модель и гейт загрузки валидирует сервис (клиенту не доверяем). Машина
    // НЕ спавнится — только регистрируется владение в памяти; спавн — через парковку.
    int ownedIndex = -1;
    const PersonalVehicleService::BuyResult result = m_personalService.buy(playerId, accountId, model, &ownedIndex);

    switch (result)
    {
    case PersonalVehicleService::BuyResult::Ok:
        // Write-through: INSERT строки владения + возврат её id (LAST_INSERT_ID той же
        // сессией того же воркер-таска — connection-scoped, корректен). dbId нужен
        // шерингу; проставляем его в память в success-колбэке с serial-guard.
        persistPurchase(playerId, accountId, model, ownedIndex);
        // Имя — из каталога (на Ok модель валидна, buy её сверил; displayName при
        // пустом имени сам даёт фолбэк «Модель {id}»). Ввод команды остаётся по id.
        player.sendClientMessage(
            INFO_COLOUR, u(fmt::format("Личная машина куплена ({}). Возьмите её на парковке",
                                       VehicleModelNames::displayName(model))));
        break;
    case PersonalVehicleService::BuyResult::LimitReached:
        player.sendClientMessage(
            ERROR_COLOUR,
            u(fmt::format("У вас уже есть личная машина (лимит {})", PersonalVehicleService::MAX_PERSONAL_VEHICLES)));
        break;
    case PersonalVehicleService::BuyResult::InvalidModel:
        player.sendClientMessage(ERROR_COLOUR, u("Недопустимая модель (400-611, кроме train carriage 569/570)"));
        break;
    case PersonalVehicleService::BuyResult::NotLoaded:
        player.sendClientMessage(ERROR_COLOUR, u("Личный транспорт ещё загружается, попробуйте через момент"));
        break;
    case PersonalVehicleService::BuyResult::PoolFull: // недостижимо: buy больше не спавнит
        player.sendClientMessage(ERROR_COLOUR, u("Пул машин переполнен"));
        break;
    case PersonalVehicleService::BuyResult::Unavailable:
        player.sendClientMessage(ERROR_COLOUR, u("Покупка сейчас недоступна"));
        break;
    }
}

void PersonalVehicleSystem::persistPurchase(int playerId, PlayerSessionService::AccountId accountId, int model,
                                            int ownedIndex)
{
    if (ownedIndex < 0)
    {
        return; // сервис не отдал индекс (не Ok) — писать нечего
    }
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    const std::uint32_t serial = session ? session->serial : 0;

    // INSERT владения + возврат dbId одной сессией того же воркер-таска: LAST_INSERT_ID
    // connection-scoped, а пул держит одну сессию на поток (см. DatabaseManager) —
    // корректный id. Возвращаем dbId (или -1 при сбое) на главный поток.
    DatabaseManager::selectQuery<long long>(
        [accountId, model](mysqlx::Schema schema) -> long long
        {
            mysqlx::Session &s = schema.getSession();
            s.sql("INSERT INTO personal_vehicle (account_id, model) VALUES (?, ?)").bind(accountId).bind(model).execute();
            mysqlx::SqlResult r = s.sql("SELECT LAST_INSERT_ID()").execute();
            mysqlx::Row row = r.fetchOne();
            return row ? row.get(0).get<std::int64_t>() : static_cast<long long>(-1);
        },
        [this, playerId, serial, ownedIndex](long long dbId)
        {
            if (dbId < 0)
            {
                return; // id не получен — запись останется dbId=-1 (шеринг недоступен до перезахода)
            }
            // Serial-guard: слот мог занять другой игрок/сессия, пока шёл async-INSERT.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            if (!m_core.getPlayers().get(playerId))
                return; // игрок вышел
            m_personalService.setDbId(playerId, ownedIndex, dbId);
        },
        [accountId, model](const std::string &error)
        {
            LogManager::log(Error, fmt::format("PersonalVehicleSystem: failed to persist vehicle (account {}, "
                                               "model {}): {}",
                                               accountId, model, error));
        });
}
