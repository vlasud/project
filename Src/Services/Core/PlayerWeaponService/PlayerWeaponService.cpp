#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fmt/format.h>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

namespace
{
// Грейс после серверной выдачи/изъятия: клиенту нужно применить RPC.
constexpr std::chrono::milliseconds SYNC_GRACE{1500};

// Не чаще этого фиксируем повторное нарушение по одному игроку (чит, который
// игнорирует снятие оружия, и так наберёт порог кика — без спама в журнал).
constexpr std::chrono::milliseconds FLAG_COOLDOWN{2000};

// Допустимый «долг» патронов: дрейф учёта на лагах и drive-by (его выстрелы не
// приходят bullet sync'ом). Стрельба глубже долга — ammo hack.
constexpr std::int32_t AMMO_DEBT = 10;

// Допустимое превышение клиентских патронов над серверными. Легальных причин почти
// нет (расход у клиента идёт не медленнее, чем списание на сервере) — запас только
// на дубли bullet sync, где сервер списал патрон дважды.
constexpr std::int64_t AMMO_EXCESS = 5;

bool rateLimited(TimePoint &lastFlag, TimePoint now)
{
    if (now - lastFlag < FLAG_COOLDOWN)
        return true;
    lastFlag = now;
    return false;
}

// --- валидация выстрела -----------------------------------------------------

// Запас дальности: цель на сервере успела уехать от места, где её видел клиент,
// плюс позиция машины — её центр, а корпус длинный (поезд, Andromada).
constexpr float RANGE_SLACK = 30.0f;
// Окно лага для допусков, масштабируемых скоростью цели (пинг + интерполяция).
constexpr float LAG_WINDOW = 0.6f;
// Origin пули не дальше этого от принятой позиции стрелка (дуло машины на ходу,
// рассинхрон последнего sync — всё в пределах; spoof обычно за сотни метров).
constexpr float ORIGIN_SLACK_SQ = 15.0f * 15.0f;
// Кредит leaky bucket: на сколько суммарно темп может опережать таблицу,
// прежде чем это перестаёт быть сгустком пакетов и становится rapid fire.
constexpr std::chrono::milliseconds ROF_BURST{1000};
// Silent aim: направление камеры сверяется с серверной геометрией «стрелок → цель».
// Начало луча — ПРИНЯТАЯ позиция стрелка, а НЕ camPos из aim sync: позицию камеры
// не проверяет ни ядро (там только длина camFrontVector), ни сервис, поэтому камерой,
// подставленной вплотную к цели, чит обнулял бы и дистанцию (уходя под
// AIM_MIN_DISTANCE), и отклонение. Из aim sync берётся ТОЛЬКО направление.
// Порог — конус наведения: угловой (растёт с дистанцией), с полом на смещение
// камеры относительно игрока и разброс, с потолком на дальних дистанциях.
constexpr float AIM_CONE_TAN = 0.20f;  // ~11° конус наведения
constexpr float AIM_MIN_OFFSET = 3.5f; // м: камера выше и позади игрока, соседняя цель под стволом
constexpr float AIM_MAX_OFFSET = 5.0f; // м: потолок конуса — на дальних дистанциях он не должен расти безгранично
// Рассинхрон позиции цели гасим упреждением по её серверной velocity, а остаточную
// ошибку упреждения (цель сменила направление) добираем допуском на скорость.
// Скорость в допуске клампится: без клампа цель в транспорте (40+ м/с) давала
// десятки метров допуска и выключала детект.
constexpr float AIM_LEAD_TIME = 0.35f;
constexpr float AIM_LAG_SLACK = 0.45f;
constexpr float AIM_SPEED_CAP = 25.0f;
constexpr float AIM_MIN_DISTANCE = 3.0f; // в упор углы не показательны

// Грейс на первые данные прицела после спавна: aim sync идёт ~30 раз в секунду, но
// первый пакет может отстать от первого выстрела. Вне грейса отсутствие данных
// прицела — само по себе нарушение (иначе «не шлю aim sync» обходит проверку).
constexpr std::chrono::seconds AIM_DATA_GRACE{3};

// Корпус стрелка: цель не может быть строго за спиной — модель в SA-MP
// разворачивается на цель (авто-прицел) либо по направлению стрельбы. Порог ~130°,
// с запасом на рассинхрон угла: последний onfoot sync мог отстать на пол-оборота.
// Опора независима от aim sync — направление модели идёт другим пакетом, и его
// подделку видят остальные игроки.
constexpr float BODY_MAX_COS = -0.64f;

// Дальность — своя таблица, НЕ WeaponInfo::range из SDK: там дальности weapon.dat
// (снайперка 100 м), а попадания в SA-MP регистрируются и заметно дальше.
float weaponRange(std::uint8_t weaponId)
{
    switch (weaponId)
    {
    case 22: // Colt45
    case 23: // Silenced
    case 24: // Deagle
    case 26: // Sawnoff
    case 28: // Uzi
    case 32: // Tec-9
        return 35.0f;
    case 25: // Shotgun
    case 27: // Spas-12
        return 40.0f;
    case 29: // MP5
        return 45.0f;
    case 30: // AK-47
        return 70.0f;
    case 38: // Minigun
        return 75.0f;
    case 31: // M4
        return 90.0f;
    case 33: // Rifle
        return 100.0f;
    case 34: // Sniper
        return 320.0f;
    default:
        return 320.0f; // незнакомое — щедро
    }
}

// Минимальный интервал между выстрелами — от РЕАЛЬНОЙ скорострельности оружия
// (WeaponInfo::shootTime, данные weapon.dat из SDK), а не от значений на глаз:
// прежняя таблица была щедрее реального темпа в 3-5 раз, и rapid fire такой
// кратности не считался нарушением.
//
// Легальный темп бывает выше табличного, отсюда делитель-запас:
//  * ROF_DIVISOR — базовый запас на лаг и сгустки пакетов;
//  * c-bug (присед отменяет анимацию после выстрела) реально ускоряет медленные
//    стволы — им запас больше;
//  * dual-wield шлёт bullet sync с двух стволов, то есть вдвое больше пакетов;
//    множитель ниже двойки сознательно — при полной двойке порог ушёл бы вдвое
//    ниже реально достижимого темпа, а сгустки пакетов и так гасит ROF_BURST.
constexpr float ROF_DIVISOR = 2.0f;
constexpr float ROF_DIVISOR_CBUG = 3.0f;
constexpr float ROF_DIVISOR_DUAL = 1.5f;
// Непулевое оружие bullet sync не шлёт (у огнемёта/спрея свой темп) — щедрый дефолт.
constexpr std::chrono::milliseconds ROF_DEFAULT{40};
constexpr std::chrono::milliseconds ROF_FLOOR{10};

bool isDualWield(std::uint8_t weaponId)
{
    return weaponId == 22 || weaponId == 26 || weaponId == 28 || weaponId == 32;
}

// Обрез (26) сюда не входит: он и так залповый, и запас ему даёт dual-множитель.
bool isCbugWeapon(std::uint8_t weaponId)
{
    return weaponId == 24 || weaponId == 25 || weaponId == 33 || weaponId == 34;
}

std::chrono::milliseconds shotInterval(std::uint8_t weaponId)
{
    const WeaponInfo &info = WeaponInfo::get(weaponId);
    if (info.type != PlayerWeaponType_Bullet || info.shootTime <= 0)
        return ROF_DEFAULT;

    float divisor = isCbugWeapon(weaponId) ? ROF_DIVISOR_CBUG : ROF_DIVISOR;
    if (isDualWield(weaponId))
        divisor *= ROF_DIVISOR_DUAL;

    const std::chrono::milliseconds interval{static_cast<int>(static_cast<float>(info.shootTime) / divisor)};
    return std::max(interval, ROF_FLOOR);
}

struct WeaponShotSpec
{
    float range;
    std::chrono::milliseconds minInterval;
};

WeaponShotSpec shotSpec(std::uint8_t weaponId)
{
    return {weaponRange(weaponId), shotInterval(weaponId)};
}

bool finite(const Vector3 &v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
} // namespace

Milliseconds PlayerWeaponService::minShotInterval(std::uint8_t weaponId)
{
    return shotSpec(weaponId).minInterval;
}

PlayerWeaponService::Slot *PlayerWeaponService::findWeapon(State &st, std::uint8_t weaponId)
{
    // slot() возвращает int8_t (-1 для бесслотового оружия: id 19/20/21 и id>=47).
    // Держим ЗНАКОВЫЙ тип: через uint8_t -1 стал бы 255 и сравнение с int8_t(-1)
    // всегда false, а st.slots[255] на массиве из MAX_WEAPON_SLOTS — OOB. Верхнюю
    // границу проверяем явно — защита от будущего вызывающего с невалидным id.
    const std::int8_t slot = WeaponSlotData(weaponId).slot();
    if (slot < 0 || slot >= MAX_WEAPON_SLOTS)
        return nullptr;
    Slot &entry = st.slots[slot];
    return entry.id == weaponId ? &entry : nullptr;
}

const PlayerWeaponService::Slot *PlayerWeaponService::findWeapon(const State &st, std::uint8_t weaponId) const
{
    const std::int8_t slot = WeaponSlotData(weaponId).slot();
    if (slot < 0 || slot >= MAX_WEAPON_SLOTS)
        return nullptr;
    const Slot &entry = st.slots[slot];
    return entry.id == weaponId ? &entry : nullptr;
}

bool PlayerWeaponService::hasWeapon(int playerId, std::uint8_t weaponId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return false;
    return findWeapon(m_state[playerId], weaponId) != nullptr;
}

int PlayerWeaponService::getAmmo(int playerId, std::uint8_t weaponId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return -1;
    const Slot *slot = findWeapon(m_state[playerId], weaponId);
    if (!slot)
        return -1;
    return slot->ammo > 0 ? slot->ammo : 0;
}

std::uint8_t PlayerWeaponService::getArmedWeapon(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return 0;
    return m_state[playerId].armed;
}

void PlayerWeaponService::giveWeapon(IPlayer &player, std::uint8_t weaponId, std::uint32_t ammo)
{
    // Знаковый тип обязателен: uint8_t превратил бы -1 в 255 и guard бы не сработал
    // (см. findWeapon) — здесь это была бы OOB-ЗАПИСЬ в st.slots[255].
    const std::int8_t slotIndex = WeaponSlotData(weaponId).slot();
    if (slotIndex < 0 || slotIndex >= MAX_WEAPON_SLOTS)
        return;

    State &st = m_state[player.getID()];
    Slot &slot = st.slots[slotIndex];
    if (slot.id == weaponId)
    {
        slot.ammo += static_cast<std::int32_t>(ammo); // как в GTA: патроны складываются
        if (slot.ammo < 0)
            slot.ammo = 0;
    }
    else
    {
        slot = {weaponId, static_cast<std::int32_t>(ammo)}; // оружие слота заменяется
    }
    st.lastChange = std::chrono::steady_clock::now();

    player.giveWeapon(WeaponSlotData{weaponId, static_cast<std::uint32_t>(slot.ammo)});
}

void PlayerWeaponService::removeWeapon(IPlayer &player, std::uint8_t weaponId)
{
    State &st = m_state[player.getID()];
    if (Slot *slot = findWeapon(st, weaponId))
    {
        *slot = {};
        st.lastChange = std::chrono::steady_clock::now();
    }
    player.removeWeapon(weaponId);
}

void PlayerWeaponService::resetWeapons(IPlayer &player)
{
    State &st = m_state[player.getID()];
    st.slots = {};
    st.armed = 0;
    st.lastChange = std::chrono::steady_clock::now();
    player.resetWeapons();
}

void PlayerWeaponService::setAmmo(IPlayer &player, std::uint8_t weaponId, std::uint32_t ammo)
{
    State &st = m_state[player.getID()];
    if (Slot *slot = findWeapon(st, weaponId))
    {
        slot->ammo = static_cast<std::int32_t>(ammo);
        st.lastChange = std::chrono::steady_clock::now();
        player.setWeaponAmmo(WeaponSlotData{weaponId, ammo});
    }
}

PlayerWeaponService::ShotOutcome PlayerWeaponService::onShot(IPlayer &player, const PlayerBulletData &bullet,
                                                             const ShotContext &ctx, TimePoint now)
{
    ShotOutcome outcome;
    State &st = m_state[player.getID()];
    const std::uint8_t weaponId = bullet.weapon;

    // Невалидный выстрел отбрасываем ВСЕГДА; в журнал пишем с рейт-лимитом.
    auto flag = [&](ShotFlag kind, std::string detail)
    {
        outcome.drop = true;
        if (!rateLimited(st.lastFlag, now))
        {
            outcome.flag = kind;
            outcome.detail = std::move(detail);
        }
    };

    // NaN/Inf: проверки границ ядра NaN проходит (NaN > b == false), а дальше
    // эти числа разойдутся по всем потребителям событий выстрела.
    if (!finite(bullet.origin) || !finite(bullet.hitPos) || !finite(bullet.offset))
    {
        flag(ShotFlag::ShotHack, fmt::format("non-finite bullet data, weapon {}", weaponId));
        return outcome;
    }

    Slot *slot = findWeapon(st, weaponId);
    if (!slot)
    {
        // Стрельба из оружия, которого сервер не выдавал.
        outcome.drop = true;
        if (!rateLimited(st.lastFlag, now))
        {
            player.removeWeapon(weaponId);
            player.setArmedWeapon(0);
            outcome.flag = ShotFlag::WeaponHack;
            outcome.detail = fmt::format("shot with unowned weapon {}", weaponId);
        }
        return outcome;
    }

    --slot->ammo;
    if (slot->ammo < -AMMO_DEBT)
    {
        // Стрельба при серверном нуле патронов глубже допуска на дрейф. Долг клампим
        // к полу (-AMMO_DEBT), а НЕ обнуляем: обнуление давало окно прощения на
        // AMMO_DEBT выстрелов, а с клампом следующий нелегальный выстрел ретриггерит
        // детект сразу.
        slot->ammo = -AMMO_DEBT;
        player.setWeaponAmmo(WeaponSlotData{weaponId, 0});
        flag(ShotFlag::WeaponHack, fmt::format("shooting weapon {} with no ammo", weaponId));
        return outcome;
    }

    const WeaponShotSpec spec = shotSpec(weaponId);

    // Темп: каждый выстрел кладёт в bucket интервал своего оружия, время его
    // осушает. Разовый сгусток пакетов после лаг-спайка съест кредит и
    // восстановится; читовый темп держит bucket у потолка — всё дропается.
    if (st.rofBucket < now - ROF_BURST)
        st.rofBucket = now - ROF_BURST;
    st.rofBucket += spec.minInterval;
    if (st.rofBucket > now + ROF_BURST)
    {
        st.rofBucket = now + ROF_BURST;
        flag(ShotFlag::RapidFire,
             fmt::format("rapid fire weapon {}: sustained rate above 1/{}ms", weaponId, spec.minInterval.count()));
        return outcome;
    }

    // Origin рядом со стрелком: silent aim часто рисует выстрел «из» цели.
    const Vector3 originDelta = bullet.origin - ctx.shooterPos;
    if (glm::dot(originDelta, originDelta) > ORIGIN_SLACK_SQ)
    {
        flag(ShotFlag::ShotHack,
             fmt::format("bullet origin {:.0f}m away from shooter, weapon {}", glm::length(originDelta), weaponId));
        return outcome;
    }

    if (ctx.targetPos)
    {
        const float targetSpeed = glm::length(ctx.targetVelocity);

        // Дальность до СЕРВЕРНОЙ позиции цели (hitPos рисует клиент — не факт).
        const float targetDistance = glm::length(*ctx.targetPos - bullet.origin);
        if (targetDistance > spec.range + RANGE_SLACK + targetSpeed * LAG_WINDOW)
        {
            flag(ShotFlag::ShotHack,
                 fmt::format("hit at {:.0f}m exceeds weapon {} range {:.0f}m", targetDistance, weaponId, spec.range));
            return outcome;
        }

        // Silent aim: камера обязана смотреть примерно в сторону цели. Оба конца
        // луча — серверные факты (позиция стрелка и позиция цели), от клиента идёт
        // только направление камеры.
        if (ctx.checkSilentAim)
        {
            const PlayerAimData &aim = player.getAimData();
            // Цель берём с упреждением: серверная позиция отстаёт от той, что видел
            // клиент, и тем сильнее, чем быстрее цель. Без упреждения допуск на
            // быструю цель пришлось бы держать в десятки метров.
            const Vector3 toTarget = *ctx.targetPos + ctx.targetVelocity * AIM_LEAD_TIME - ctx.shooterPos;
            const float distance = glm::length(toTarget);
            const float frontLen = glm::length(aim.camFrontVector);
            const bool aimUsable = finite(aim.camFrontVector) && frontLen > 0.1f;

            if (!aimUsable)
            {
                // Данных прицела нет: раньше проверка молча пропускалась, и клиент,
                // который не шлёт aim sync, обходил её бесплатно. Прощаем только
                // первые секунды после спавна — первый aim sync мог не успеть.
                if (now - st.spawnAt > AIM_DATA_GRACE)
                {
                    flag(ShotFlag::SilentAim, fmt::format("hit player without aim data, weapon {}", weaponId));
                    return outcome;
                }
            }
            else if (finite(toTarget) && distance > AIM_MIN_DISTANCE)
            {
                const float along = glm::dot(aim.camFrontVector / frontLen, toTarget);
                const float cone = std::max(std::min(distance * AIM_CONE_TAN, AIM_MAX_OFFSET), AIM_MIN_OFFSET);
                const float threshold = cone + std::min(targetSpeed, AIM_SPEED_CAP) * AIM_LAG_SLACK;
                const float offAxisSq = glm::dot(toTarget, toTarget) - along * along;
                if (along < 0.0f || offAxisSq > threshold * threshold)
                {
                    flag(ShotFlag::SilentAim,
                         fmt::format("hit player {:.0f}m off aim ray at {:.0f}m, weapon {}",
                                     along < 0.0f ? distance : std::sqrt(std::max(offAxisSq, 0.0f)), distance,
                                     weaponId));
                    return outcome;
                }

                // Корпус стрелка — вторая опора, не зависящая от aim sync: цель
                // строго за спиной означает, что данные прицела подделаны (сам
                // разворот модели скрыть нельзя, его видят остальные игроки).
                const float yaw = player.getRotation().ToEuler().z;
                const Vector3 flat{toTarget.x, toTarget.y, 0.0f};
                const float flatLen = glm::length(flat);
                if (std::isfinite(yaw) && flatLen > 0.001f)
                {
                    const float radians = glm::radians(yaw);
                    const Vector3 facing{-std::sin(radians), std::cos(radians), 0.0f};
                    if (glm::dot(facing, flat / flatLen) < BODY_MAX_COS)
                    {
                        flag(ShotFlag::SilentAim,
                             fmt::format("hit player {:.0f}m behind shooter's back, weapon {}", distance, weaponId));
                        return outcome;
                    }
                }
            }
        }
    }
    return outcome;
}

PlayerWeaponService::Outcome PlayerWeaponService::verifySync(IPlayer &player, TimePoint now)
{
    Outcome outcome;
    State &st = m_state[player.getID()];

    // Оружие в руках — первым: если оно уже чужое, разбирать патроны незачем.
    if (!verifyArmedWeapon(player, st, now, outcome))
        return outcome;

    verifyAmmo(player, st, now, outcome);
    return outcome;
}

bool PlayerWeaponService::verifyArmedWeapon(IPlayer &player, State &st, TimePoint now, Outcome &outcome)
{
    const std::uint8_t reported = static_cast<std::uint8_t>(player.getArmedWeapon());

    // Кулаки — всегда; парашют игра выдаёт сама при прыжке из самолёта;
    // детонатор появляется вместе с выданными сатчелами.
    if (reported == 0 || reported == 46 || (reported == 40 && findWeapon(st, 39)))
    {
        st.armed = reported;
        return true;
    }

    if (findWeapon(st, reported))
    {
        st.armed = reported;
        return true;
    }

    // В руках оружие, которого нет в серверном инвентаре.
    if (now - st.lastChange < SYNC_GRACE)
        return false; // клиент ещё применяет недавнюю выдачу/изъятие

    if (!rateLimited(st.lastFlag, now))
    {
        player.removeWeapon(reported);
        player.setArmedWeapon(0);
        outcome.weaponHack = true;
        outcome.detail = fmt::format("armed unowned weapon {}", reported);
    }
    return false;
}

void PlayerWeaponService::verifyAmmo(IPlayer &player, State &st, TimePoint now, Outcome &outcome)
{
    // Грейс серверных операций: после giveWeapon/setAmmo клиент ещё не применил RPC,
    // его патроны отстают — принять это отставание за правду значило бы съесть
    // только что выданное.
    if (now - st.lastChange < SYNC_GRACE)
        return;

    const WeaponSlots &clientSlots = player.getWeapons();
    for (std::size_t index = 0; index < MAX_WEAPON_SLOTS; ++index)
    {
        Slot &server = st.slots[index];
        if (server.id == 0)
            continue; // серверный слот пуст: лишнее оружие ловит проверка «в руках»

        const WeaponSlotData &client = clientSlots[index];
        if (client.id != server.id)
            continue; // в слоте другое оружие — это тоже случай проверки «в руках»

        // В int64: клиент присылает uint32, и 0xFFFFFFFF в int32 стал бы -1, то есть
        // хак с максимумом патронов выглядел бы как их расход.
        const std::int64_t clientAmmo = static_cast<std::int64_t>(client.ammo);
        // Долг (отрицательный серверный остаток) сравниваем как ноль: клиент,
        // показывающий 0 после стрельбы в долг, ничего не подделывал.
        const std::int64_t serverAmmo = server.ammo > 0 ? server.ammo : 0;

        if (clientAmmo > serverAmmo + AMMO_EXCESS)
        {
            // Патроны выросли без серверной выдачи — ammo hack. Ловится СРАЗУ, не
            // дожидаясь, пока чит отстреляет легальный запас.
            if (!rateLimited(st.lastFlag, now))
            {
                outcome.weaponHack = true;
                outcome.detail =
                    fmt::format("weapon {} ammo {} over server {}", server.id, clientAmmo, serverAmmo);
            }
            player.setWeaponAmmo(WeaponSlotData{server.id, static_cast<std::uint32_t>(serverAmmo)});
            st.lastChange = now; // окно на применение отката, иначе форс каждый апдейт
            return;
        }

        if (clientAmmo < server.ammo)
        {
            // Клиент потратил больше, чем сервер успел списать: drive-by bullet sync
            // не шлёт, пакеты теряются. Снижение принимаем за правду — занижать себе
            // патроны читу невыгодно, а иначе рассинхрон копится и прячет хак.
            server.ammo = static_cast<std::int32_t>(clientAmmo);
        }
    }
}

void PlayerWeaponService::onSpawn(IPlayer &player)
{
    // GTA теряет оружие на смерти — серверный инвентарь чистится, игровая логика
    // перевыдаёт через сервис.
    State &st = m_state[player.getID()];
    st.slots = {};
    st.armed = 0;
    st.lastChange = std::chrono::steady_clock::now();
    st.spawnAt = st.lastChange; // отсюда идёт грейс на первые данные прицела
}

void PlayerWeaponService::reset(int playerId)
{
    if (!validPlayerId(playerId))
        return;
    m_state[playerId] = State{};
}

void PlayerWeaponService::getWeapons(int playerId, std::vector<std::pair<std::uint8_t, int>> &out) const
{
    out.clear();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    for (const Slot &slot : m_state[playerId].slots)
    {
        if (slot.id == 0)
            continue; // пустой слот
        out.emplace_back(slot.id, slot.ammo > 0 ? slot.ammo : 0);
    }
}
