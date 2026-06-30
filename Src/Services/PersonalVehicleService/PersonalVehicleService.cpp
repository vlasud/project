#include "Services/PersonalVehicleService/PersonalVehicleService.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>

namespace
{
bool validPlayer(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}

// Диапазон моделей машин SA — тот же, что форсит VehicleService::create. Валидируем
// и здесь, чтобы мусорная модель не уходила в SDK и давала InvalidModel.
bool validModel(int model)
{
    // Train carriage (569 freiflat, 570 streakc) ядро ОТКАЗЫВАЕТСЯ уничтожать
    // (release() для isTrainCarriage делает early-return): такая машина не исчезла бы
    // на reset, осталась бы в пуле — утечка машин. Не даём их купить.
    if (model == 569 || model == 570)
    {
        return false;
    }
    return model >= 400 && model <= 611;
}

// Пустой список владения для bounds-промаха owned(): отдаём ссылку на статический
// пустой вектор (вызывающий итерируется без ветвления на nullptr).
const std::vector<PersonalVehicleService::OwnedVehicle> &emptyOwned()
{
    static const std::vector<PersonalVehicleService::OwnedVehicle> empty;
    return empty;
}
} // namespace

void PersonalVehicleService::bind(VehicleService &vehicleService)
{
    m_vehicleService = &vehicleService;
}

PersonalVehicleService::BuyResult PersonalVehicleService::buy(int playerId, AccountId accountId, int model)
{
    if (!validPlayer(playerId) || !m_vehicleService || accountId == PlayerSessionService::NO_ACCOUNT)
    {
        return BuyResult::Unavailable; // нет игрока/сервиса/аккаунта — покупать некому/нечем
    }
    // Гейт старт-гонки: пока владение из БД не легло в память, лимит считать не по
    // чему — купленная модель + загруженная позже обошли бы его. Покупка отложена.
    if (!m_loaded[playerId])
    {
        return BuyResult::NotLoaded;
    }

    std::vector<OwnedVehicle> &owned = m_owned[playerId];
    // Лимит форсится здесь (серверная политика), а не на клиенте.
    if (static_cast<int>(owned.size()) >= MAX_PERSONAL_VEHICLES)
    {
        return BuyResult::LimitReached;
    }
    // Модель валидируем до SDK — невалидная не даст спавнить через парковку.
    if (!validModel(model))
    {
        return BuyResult::InvalidModel;
    }

    // Память оптимистична (как FactionService::setMember): сразу регистрируем
    // ВЛАДЕНИЕ, машина не создаётся (спавн — через парковку).
    owned.push_back(OwnedVehicle{model, -1});

    // Write-through: право владения уходит в БД сразу (переживёт перезаход). id строки
    // (AUTO_INCREMENT) в in-memory владении пока не нужен — лимит/спавн на нём не
    // завязаны. Ошибка БД лишь логируется (память уже обновлена).
    DatabaseManager::throwQuery(
        [accountId, model](mysqlx::Schema schema)
        {
            schema.getTable("personal_vehicle").insert("account_id", "model").values(accountId, model).execute();
        },
        [accountId, model](const std::string &error)
        {
            LogManager::log(Error, fmt::format("PersonalVehicleService: failed to persist vehicle (account {}, "
                                               "model {}): {}",
                                               accountId, model, error));
        });
    return BuyResult::Ok;
}

const std::vector<PersonalVehicleService::OwnedVehicle> &PersonalVehicleService::owned(int playerId) const
{
    if (!validPlayer(playerId))
    {
        return emptyOwned();
    }
    return m_owned[playerId];
}

PersonalVehicleService::SpawnResult PersonalVehicleService::spawn(int playerId, int ownedIndex, Vector3 position,
                                                                  float angle, int colour1, int colour2,
                                                                  IVehicle **out)
{
    if (!validPlayer(playerId) || !m_vehicleService)
    {
        return SpawnResult::Unavailable;
    }
    std::vector<OwnedVehicle> &owned = m_owned[playerId];
    if (ownedIndex < 0 || ownedIndex >= static_cast<int>(owned.size()))
    {
        return SpawnResult::BadIndex;
    }

    OwnedVehicle &entry = owned[ownedIndex];

    // Пере-спавн: экземпляр уже стоит — уничтожаем старый. destroy СИНХРОННО
    // триггерит onWorldVehicleDestroyed по старому id, который обнулит entry.vehicleId
    // (запись НЕ удаляется — индекс/ссылка entry стабильны), мы ниже пишем новый id.
    if (entry.vehicleId != -1)
    {
        m_vehicleService->destroy(entry.vehicleId);
        entry.vehicleId = -1; // на случай, если наблюдатель не сработал (страховка)
    }

    // Машина — только через VehicleService::create (тег Owner::Player, ownerId =
    // playerId). nullptr — пул машин ядра исчерпан; старую (если был пере-спавн) уже
    // уничтожили, vehicleId остался -1, владение цело.
    IVehicle *vehicle =
        m_vehicleService->create(entry.model, position, angle, colour1, colour2, VehicleService::Owner::Player,
                                 playerId);
    if (!vehicle)
    {
        return SpawnResult::PoolFull;
    }

    entry.vehicleId = vehicle->getID();
    if (out)
    {
        *out = vehicle;
    }
    return SpawnResult::Ok;
}

int PersonalVehicleService::currentVehicle(int playerId, int ownedIndex) const
{
    if (!validPlayer(playerId))
    {
        return -1;
    }
    const std::vector<OwnedVehicle> &owned = m_owned[playerId];
    if (ownedIndex < 0 || ownedIndex >= static_cast<int>(owned.size()))
    {
        return -1;
    }
    return owned[ownedIndex].vehicleId;
}

int PersonalVehicleService::count(int playerId) const
{
    if (!validPlayer(playerId))
    {
        return 0;
    }
    return static_cast<int>(m_owned[playerId].size());
}

bool PersonalVehicleService::atLimit(int playerId) const
{
    return count(playerId) >= MAX_PERSONAL_VEHICLES;
}

void PersonalVehicleService::load(int playerId, const std::vector<int> &models)
{
    if (!validPlayer(playerId))
    {
        return;
    }
    // Зеркало из БД: память перед этим уже пуста (reset на коннекте/конце прошлой
    // сессии). Кладём владения БЕЗ записи в БД (это загрузка, не покупка). Модель из
    // БД фильтруем тем же validModel — мусорная/устаревшая запись не даст спавнить.
    std::vector<OwnedVehicle> &owned = m_owned[playerId];
    owned.clear();
    for (const int model : models)
    {
        if (static_cast<int>(owned.size()) >= MAX_PERSONAL_VEHICLES)
        {
            break; // лимит мог уменьшиться в коде — лишние строки БД просто не грузим
        }
        if (validModel(model))
        {
            owned.push_back(OwnedVehicle{model, -1});
        }
    }
    m_loaded[playerId] = true; // зеркало легло — покупка разблокирована
}

void PersonalVehicleService::reset(int playerId)
{
    if (!validPlayer(playerId) || !m_vehicleService)
    {
        return;
    }

    // destroy(id) СИНХРОННО триггерит наблюдателя subscribeDestroyed ->
    // onWorldVehicleDestroyed(id), который ищет id ПО ВСЕМ игрокам и модифицирует
    // m_owned. Поэтому: собираем id заспавненных экземпляров локально и ОЧИЩАЕМ
    // владение игрока ДО вызовов destroy — наблюдатель увидит уже пустой слот этого
    // игрока (по нему — no-op), а мы итерируемся по своей копии id.
    std::vector<int> toDestroy;
    toDestroy.reserve(m_owned[playerId].size());
    for (const OwnedVehicle &entry : m_owned[playerId])
    {
        if (entry.vehicleId != -1)
        {
            toDestroy.push_back(entry.vehicleId);
        }
    }
    // Память владения и машины — СЕССИОННЫЕ: гасятся на конце/начале сессии. БД НЕ
    // трогаем — право владения остаётся в personal_vehicle, подтянется на старте.
    m_owned[playerId].clear();
    m_loaded[playerId] = false; // следующая покупка ждёт нового зеркала из БД

    for (const int vehicleId : toDestroy)
    {
        m_vehicleService->destroy(vehicleId);
    }
}

void PersonalVehicleService::onWorldVehicleDestroyed(int vehicleId)
{
    if (vehicleId < 0)
    {
        return; // несуществующий id — искать нечего
    }
    // Машина пропала (взрыв/destroy/респаун-цикл) — обнуляем её id во владении, где
    // он есть, НЕ удаляя запись (владение сохраняется, машину спавнят заново через
    // парковку). Линейно по игрокам (мало; редкое событие уничтожения).
    for (auto &owned : m_owned)
    {
        for (OwnedVehicle &entry : owned)
        {
            if (entry.vehicleId == vehicleId)
            {
                entry.vehicleId = -1;
                return; // id уникален среди заспавненных — дальше искать незачем
            }
        }
    }
}
