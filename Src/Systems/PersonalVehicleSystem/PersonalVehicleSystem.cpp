#include "Systems/PersonalVehicleSystem/PersonalVehicleSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
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
    // disconnect). Старт: грузим модели аккаунта из БД. Конец: reset (уничтожить
    // машины + очистить ПАМЯТЬ владения; право владения остаётся в БД).
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session) { loadOwnership(player, session); });
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            const int playerId = player.getID();
            if (playerId < 0 || playerId >= MAX_PLAYERS)
                return;
            m_personalService.reset(playerId);
        });

    // Машину могли уничтожить извне (взрыв, другая система, респаун-цикл, /destroyveh):
    // обнуляем её id во владении (владение остаётся, машину спавнят заново через
    // парковку) — нет висячих id и двойного destroy.
    m_vehicleService.subscribeDestroyed(
        [this](IVehicle &vehicle) { m_personalService.onWorldVehicleDestroyed(vehicle.getID()); });

    // Смерть машины (HP -> 0): личную машину удаляем, чтобы она ПРОПАДАЛА, а не
    // висела мёртвым вреком и НЕ вернулась. Машину «возвращает» death-респавн ядра по
    // ГЛОБАЛЬНОМУ конфигу game.vehicle_respawn_time (НЕ по respawnDelay=-1 — тот гасит
    // лишь респавн по простою); немедленный destroy убирает её до этого таймера, в том
    // же тике. Урон её не взрывает (глохнет как все — анти-грифинг в VehicleService),
    // значит реальная смерть — редкий мгновенный подрыв вплотную, сервер его не ловит.
    m_vehicleService.subscribeDied([this](IVehicle &vehicle) { onVehicleDied(vehicle); });

    auto &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add("pvbuy", {{PlayerCommandService::Param::Int, "id модели"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 { buyDebug(player, args.getInt(0)); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL),
                 "дебаг: купить личную машину по id модели", PlayerCommandService::HelpCategory::Hidden);
}

void PersonalVehicleSystem::loadOwnership(IPlayer &player, const PlayerSessionService::Session &session)
{
    // SELECT моделей владения аккаунта; запрос И вычитка — на воркере (возвращаем
    // владеющий vector<int>, mysqlx-объект границу потока не пересекает).
    DatabaseManager::selectQuery<std::vector<int>>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult result = schema.getTable("personal_vehicle")
                                           .select("model")
                                           .where("account_id = :account")
                                           .bind("account", accountId)
                                           .execute();
            std::vector<int> models;
            for (mysqlx::Row row = result.fetchOne(); row; row = result.fetchOne())
            {
                models.push_back(row.get(0).get<int>());
            }
            return models;
        },
        [this, playerId = player.getID(), serial = session.serial](std::vector<int> models)
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
            m_personalService.load(playerId, models);
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

void PersonalVehicleSystem::onVehicleDied(IVehicle &vehicle)
{
    const int vehicleId = vehicle.getID();
    // Только ЛИЧНАЯ машина пропадает при смерти; чужие owner-теги (Faction/Work)
    // решают свою политику смерти сами. getOwner — серверный тег, O(1).
    if (m_vehicleService.getOwner(vehicleId) != VehicleService::Owner::Player)
        return;
    // destroy зовётся из died-наблюдателя внутри события смерти — машина залочена в
    // пуле, release откладывается до unlock (SDK MarkedPoolStorage): диспатч смерти
    // не рвётся, onVehicleDestroyed придёт позже и обнулит vehicleId владения
    // (владение СОХРАНЯЕТСЯ) — спавн заново только вручную на парковке.
    m_vehicleService.destroy(vehicleId);
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
    // НЕ спавнится — только регистрируется владение (write-through в БД); спавн —
    // через парковку.
    const PersonalVehicleService::BuyResult result = m_personalService.buy(playerId, accountId, model);

    switch (result)
    {
    case PersonalVehicleService::BuyResult::Ok:
        player.sendClientMessage(
            INFO_COLOUR, u(fmt::format("Личная машина куплена (модель {}). Возьмите её на парковке", model)));
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
