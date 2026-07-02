#include "Services/PersonalVehicleService/PersonalVehicleService.h"

#include <cmath>

namespace
{
bool validPlayer(int playerId)
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}

// Кламп персистентного fuel 0..CAP; мусор (NaN/Inf) -> дефолт «нет сохранённого».
float clampFuel(double raw)
{
    if (!std::isfinite(raw))
    {
        return -1.0f; // мусор из БД — трактуем как «не сохранено», spawn даст полный бак
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

PersonalVehicleService::BuyResult PersonalVehicleService::buy(int playerId, AccountId accountId, int model,
                                                             int *outIndex)
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
    // ВЛАДЕНИЕ (dbId=-1, проставит система из LAST_INSERT_ID), машина не создаётся
    // (спавн — через парковку). Write-through INSERT делает система (ей нужен dbId).
    if (outIndex)
    {
        *outIndex = static_cast<int>(owned.size());
    }
    owned.push_back(OwnedVehicle{model, -1, -1});
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

    // Пере-спавн: экземпляр уже стоит — СНАЧАЛА снимаем его остаток топлива (иначе
    // «убрать в гараж -> вызвать заново» доливало бы бак бесплатно, см. Docs/
    // GameDesign/Economy.md «Задел на будущий сток»), ПОТОМ уничтожаем старый. destroy
    // СИНХРОННО триггерит onWorldVehicleDestroyed по старому id, который обнулит
    // entry.vehicleId (запись НЕ удаляется — индекс/ссылка entry стабильны), мы ниже
    // пишем новый id.
    if (entry.vehicleId != -1)
    {
        entry.fuel = m_vehicleService->getFuel(entry.vehicleId);
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

    // Применить персистентный остаток поверх дефолтного полного бака от create:
    // entry.fuel < 0 — ещё не сохранён (первая покупка/не грузился) — оставляем полный.
    if (entry.fuel >= 0.0f)
    {
        m_vehicleService->setFuel(*vehicle, entry.fuel);
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

long long PersonalVehicleService::dbIdOf(int playerId, int ownedIndex) const
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
    return owned[ownedIndex].dbId;
}

void PersonalVehicleService::setFuel(int playerId, int ownedIndex, float fuel)
{
    if (!validPlayer(playerId) || !std::isfinite(fuel))
    {
        return; // мусорный float не оседает в снимке
    }
    std::vector<OwnedVehicle> &owned = m_owned[playerId];
    if (ownedIndex < 0 || ownedIndex >= static_cast<int>(owned.size()))
    {
        return;
    }
    owned[ownedIndex].fuel =
        fuel < 0.0f ? 0.0f : (fuel > VehicleService::FUEL_CAPACITY ? VehicleService::FUEL_CAPACITY : fuel);
}

float PersonalVehicleService::fuelOf(int playerId, int ownedIndex) const
{
    if (!validPlayer(playerId))
    {
        return -1.0f;
    }
    const std::vector<OwnedVehicle> &owned = m_owned[playerId];
    if (ownedIndex < 0 || ownedIndex >= static_cast<int>(owned.size()))
    {
        return -1.0f;
    }
    return owned[ownedIndex].fuel;
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

bool PersonalVehicleService::detach(long long dbId)
{
    if (dbId < 0)
    {
        return false; // мусорный/непроставленный dbId — искать нечего
    }
    // dbId уникален глобально -> линейный проход по всем игрокам (как
    // onWorldVehicleDestroyed, холодный путь). Обнуляем vehicleId БЕЗ destroy: reset на
    // дисконнекте владельца теперь не тронет припаркованную (её id уже -1).
    for (auto &owned : m_owned)
    {
        for (OwnedVehicle &entry : owned)
        {
            if (entry.dbId == dbId)
            {
                entry.vehicleId = -1;
                return true;
            }
        }
    }
    return false;
}

bool PersonalVehicleService::attach(long long dbId, int vehicleId)
{
    if (dbId < 0)
    {
        return false;
    }
    // Снятие с парковки: вернуть живой экземпляр под сессионный жизненный цикл владения.
    for (auto &owned : m_owned)
    {
        for (OwnedVehicle &entry : owned)
        {
            if (entry.dbId == dbId)
            {
                entry.vehicleId = vehicleId;
                return true;
            }
        }
    }
    return false;
}

void PersonalVehicleService::load(int playerId, const std::vector<std::tuple<long long, int, double>> &rows)
{
    if (!validPlayer(playerId))
    {
        return;
    }
    // Зеркало из БД: память перед этим уже пуста (reset на коннекте/конце прошлой
    // сессии). Кладём владения БЕЗ записи в БД (это загрузка, не покупка). Модель из
    // БД фильтруем тем же validModel — мусорная/устаревшая запись не даст спавнить.
    // dbId (id строки) сохраняем — по нему шеринг ссылается на конкретную машину.
    // fuel клампится 0..CAP (мусор из БД — NaN/отрицательное/сверх капасити).
    std::vector<OwnedVehicle> &owned = m_owned[playerId];
    owned.clear();
    for (const auto &[dbId, model, fuel] : rows)
    {
        if (static_cast<int>(owned.size()) >= MAX_PERSONAL_VEHICLES)
        {
            break; // лимит мог уменьшиться в коде — лишние строки БД просто не грузим
        }
        if (validModel(model))
        {
            owned.push_back(OwnedVehicle{model, -1, dbId, clampFuel(fuel)});
        }
    }
    m_loaded[playerId] = true; // зеркало легло — покупка разблокирована
}

void PersonalVehicleService::setDbId(int playerId, int ownedIndex, long long dbId)
{
    if (!validPlayer(playerId))
    {
        return;
    }
    std::vector<OwnedVehicle> &owned = m_owned[playerId];
    if (ownedIndex < 0 || ownedIndex >= static_cast<int>(owned.size()))
    {
        return; // индекс сдвинулся (reset/reload между запуском и колбэком) — no-op
    }
    // Не перетираем уже присвоенный id (загрузка могла лечь поверх, если игрок
    // перезашёл): проставляем только «пустой» dbId.
    if (owned[ownedIndex].dbId == -1)
    {
        owned[ownedIndex].dbId = dbId;
    }
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
