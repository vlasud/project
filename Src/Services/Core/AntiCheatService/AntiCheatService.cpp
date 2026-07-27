#include "Services/Core/AntiCheatService/AntiCheatService.h"

#include <algorithm>

namespace
{
// Вес нарушения в долях порога: 0.25 — «четыре таких отключают». Тяжёлые весят
// больше не по опасности, а по достоверности: чем меньше у детекта шансов на
// ложное срабатывание, тем дороже одно событие.
//
// Дорогие (2-3 события): подделка состояния, которую лаги не имитируют.
// Дешёвые (5 событий): пороговые детекты, где лаг и рельеф дают шум.
float defaultWeight(AntiCheatService::ViolationType type)
{
    switch (type)
    {
    case AntiCheatService::ViolationType::DeathEvasion:
    case AntiCheatService::ViolationType::CarShot:
        return 0.5f; // два события
    case AntiCheatService::ViolationType::HealthHack:
    case AntiCheatService::ViolationType::WeaponHack:
    case AntiCheatService::ViolationType::SpecialActionHack:
    case AntiCheatService::ViolationType::SpawnHack:
    case AntiCheatService::ViolationType::VehicleEjectEvasion:
        return 0.34f; // три события
    case AntiCheatService::ViolationType::DamageHack:
    case AntiCheatService::ViolationType::ShotHack:
    case AntiCheatService::ViolationType::SilentAim:
    case AntiCheatService::ViolationType::QuickTurn:
    case AntiCheatService::ViolationType::StateHack:
    case AntiCheatService::ViolationType::VehicleHack:
        return 0.25f; // четыре события
    case AntiCheatService::ViolationType::ForcedAnimationEscape:
    case AntiCheatService::ViolationType::TeleportHack:
    case AntiCheatService::ViolationType::SpeedHack:
    case AntiCheatService::ViolationType::RapidFire:
    case AntiCheatService::ViolationType::PickupHack:
    case AntiCheatService::ViolationType::CheckpointHack:
        return 0.2f; // пять событий
    case AntiCheatService::ViolationType::Count:
        break;
    }
    return 0.2f;
}
} // namespace

const char *AntiCheatService::name(ViolationType type)
{
    switch (type)
    {
    case ViolationType::ForcedAnimationEscape:
        return "ForcedAnimationEscape";
    case ViolationType::HealthHack:
        return "HealthHack";
    case ViolationType::DamageHack:
        return "DamageHack";
    case ViolationType::DeathEvasion:
        return "DeathEvasion";
    case ViolationType::TeleportHack:
        return "TeleportHack";
    case ViolationType::SpeedHack:
        return "SpeedHack";
    case ViolationType::StateHack:
        return "StateHack";
    case ViolationType::SpecialActionHack:
        return "SpecialActionHack";
    case ViolationType::WeaponHack:
        return "WeaponHack";
    case ViolationType::ShotHack:
        return "ShotHack";
    case ViolationType::RapidFire:
        return "RapidFire";
    case ViolationType::SilentAim:
        return "SilentAim";
    case ViolationType::VehicleHack:
        return "VehicleHack";
    case ViolationType::PickupHack:
        return "PickupHack";
    case ViolationType::CheckpointHack:
        return "CheckpointHack";
    case ViolationType::SpawnHack:
        return "SpawnHack";
    case ViolationType::CarShot:
        return "CarShot";
    case ViolationType::QuickTurn:
        return "QuickTurn";
    case ViolationType::VehicleEjectEvasion:
        return "VehicleEjectEvasion";
    case ViolationType::Count:
        break;
    }
    // switch исчерпывающий по всем ViolationType (без default) — добавление нового
    // значения enum ловит -Wswitch на этапе компиляции.
    return "Unknown";
}

void AntiCheatService::record(int playerId, ViolationType type, std::string detail, TimePoint now)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS || type == ViolationType::Count)
        return;

    // Боты (NPC) под детекторы не подпадают: сервер сам двигает их рывками, и это
    // штатно выглядит как телепорт или спидхак. Проверка одна на запись — записи
    // редкие, на горячий путь это не влияет.
    if (m_botCheck && m_botCheck(playerId))
        return;

    PlayerRecord &record = m_records[playerId];
    // Счёт сессии: вес нарушения с личным множителем игрока (выше единицы у тех,
    // кого анти-чит уже ловил) и с надбавкой за отсутствие админов онлайн.
    record.score += weight(type) * record.multiplier * (aggressive() ? m_noAdminMultiplier : 1.0f);
    if (record.total == 0)
        record.firstAt = now;
    ++record.total;
    record.lastAt = now;

    // Храним только последние RECENT_LIMIT записей с деталями: спамящий читер не
    // должен раздувать память без предела. Полный счётчик total при этом не теряется.
    record.recent.push_back({type, now, std::move(detail)});
    if (record.recent.size() > RECENT_LIMIT)
        record.recent.erase(record.recent.begin());

    for (const Observer &observer : m_observers)
        observer(playerId, type, record);
}

void AntiCheatService::subscribe(Observer observer)
{
    m_observers.push_back(std::move(observer));
}

const AntiCheatService::PlayerRecord &AntiCheatService::get(int playerId) const
{
    static const PlayerRecord empty;
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return empty;
    return m_records[playerId];
}

std::uint32_t AntiCheatService::count(int playerId) const
{
    return get(playerId).total;
}

bool AntiCheatService::flagged(int playerId) const
{
    return count(playerId) > 0;
}

void AntiCheatService::clear(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    // Множитель переживает очистку журнала: он про аккаунт (персист в БД), а не
    // про текущую серию нарушений.
    const float keepMultiplier = m_records[playerId].multiplier;
    m_records[playerId] = PlayerRecord{};
    m_records[playerId].multiplier = keepMultiplier;
}

float AntiCheatService::weight(ViolationType type) const
{
    const std::size_t index = static_cast<std::size_t>(type);
    if (index >= TYPE_COUNT)
        return 0.0f;
    // Ноль в таблице — она ещё не инициализирована (сервис создан, resetTuning не
    // звали): отдаём дефолт кода, чтобы детекты не остались без веса.
    return m_weights[index] > 0.0f ? m_weights[index] : defaultWeight(type);
}

void AntiCheatService::setWeight(ViolationType type, float value)
{
    const std::size_t index = static_cast<std::size_t>(type);
    if (index >= TYPE_COUNT)
        return;
    m_weights[index] = std::clamp(value, 0.0f, 10.0f);
}

float AntiCheatService::threshold() const
{
    return m_threshold;
}

void AntiCheatService::setThreshold(float value)
{
    // Ноль или отрицательный порог отключал бы игроков на первом же событии.
    m_threshold = std::clamp(value, 0.05f, 100.0f);
}

float AntiCheatService::kickedMultiplier() const
{
    return m_kickedMultiplier;
}

void AntiCheatService::setKickedMultiplier(float value)
{
    // Ниже единицы множитель делал бы пойманного терпимее обычного игрока.
    m_kickedMultiplier = std::clamp(value, 1.0f, 10.0f);
}

float AntiCheatService::noAdminMultiplier() const
{
    return m_noAdminMultiplier;
}

void AntiCheatService::setNoAdminMultiplier(float value)
{
    // Ниже единицы надбавка делала бы игру без админов терпимее — обратное тому,
    // ради чего режим существует.
    m_noAdminMultiplier = std::clamp(value, 1.0f, 10.0f);
}

void AntiCheatService::setAdminPresenceCheck(AdminPresenceCheck check)
{
    m_adminPresence = std::move(check);
}

void AntiCheatService::setBotCheck(BotCheck check)
{
    m_botCheck = std::move(check);
}

bool AntiCheatService::aggressive() const
{
    // Нет предиката — режим выключен: сервис не должен угадывать за систему.
    return m_adminPresence && !m_adminPresence();
}

float AntiCheatService::score(int playerId) const
{
    return get(playerId).score;
}

void AntiCheatService::resetScore(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_records[playerId].score = 0.0f;
}

float AntiCheatService::multiplier(int playerId) const
{
    return get(playerId).multiplier;
}

void AntiCheatService::setMultiplier(int playerId, float value)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
        return;
    m_records[playerId].multiplier = std::clamp(value, 1.0f, 10.0f);
}

void AntiCheatService::resetTuning()
{
    for (std::size_t index = 0; index < TYPE_COUNT; ++index)
        m_weights[index] = defaultWeight(static_cast<ViolationType>(index));
    m_threshold = 1.0f;
    m_kickedMultiplier = 1.2f;
    m_noAdminMultiplier = 1.2f;
    // Предикат присутствия админов не трогаем: он не настройка, а связь с системой.
}
