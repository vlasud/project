#include "Systems/Core/AntiCheatSystem/AntiCheatSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <string>
#include <string_view>
#include <vector>

namespace
{
// Политика: у каждого нарушения свой вес, счёт копится за сессию и множится на
// личный множитель игрока (AntiCheatService). Дошёл до порога — отключаем.
// Одиночная запись (лаг, пограничный случай) кика не вызывает: вес одного события
// заведомо меньше порога, а активный чит добирает его за секунды.

// Канал [AC] в чат админам — серым: это фоновый поток наблюдений, он не должен
// перебивать по заметности ни админ-чат, ни игровые сообщения.
const Colour AC_COLOUR{170, 170, 170};

// Сколько игрок видит диалог с причиной до разрыва соединения.
constexpr Milliseconds KICK_DELAY{4000};

// Человеческая формулировка типа нарушения — для окна игроку. Намеренно без
// порогов и чисел: категории игроку достаточно, а читеру знать, на чём именно он
// спалился, не нужно. switch исчерпывающий (без default) — новый тип ловит -Wswitch.
const char *reasonText(AntiCheatService::ViolationType type)
{
    switch (type)
    {
    case AntiCheatService::ViolationType::ForcedAnimationEscape:
        return "выход из служебной анимации";
    case AntiCheatService::ViolationType::HealthHack:
        return "изменение здоровья или брони";
    case AntiCheatService::ViolationType::DamageHack:
        return "недостоверный урон по игрокам";
    case AntiCheatService::ViolationType::DeathEvasion:
        return "игра после смерти";
    case AntiCheatService::ViolationType::TeleportHack:
        return "перемещение вне правил игры";
    case AntiCheatService::ViolationType::SpeedHack:
        return "превышение возможной скорости";
    case AntiCheatService::ViolationType::StateHack:
        return "недопустимое действие с транспортом";
    case AntiCheatService::ViolationType::SpecialActionHack:
        return "недоступное состояние персонажа";
    case AntiCheatService::ViolationType::WeaponHack:
        return "оружие или патроны вне выдачи сервера";
    case AntiCheatService::ViolationType::ShotHack:
        return "недостоверные данные выстрела";
    case AntiCheatService::ViolationType::RapidFire:
        return "темп стрельбы выше возможного";
    case AntiCheatService::ViolationType::SilentAim:
        return "стрельба без наведения на цель";
    case AntiCheatService::ViolationType::VehicleHack:
        return "изменение состояния транспорта";
    case AntiCheatService::ViolationType::PickupHack:
        return "подбор предмета на расстоянии";
    case AntiCheatService::ViolationType::CheckpointHack:
        return "срабатывание метки на расстоянии";
    case AntiCheatService::ViolationType::SpawnHack:
        return "возрождение вне правил игры";
    case AntiCheatService::ViolationType::CarShot:
        return "стрельба из транспорта недоступным оружием";
    case AntiCheatService::ViolationType::QuickTurn:
        return "мгновенные развороты персонажа";
    }
    return "недопустимые действия клиента";
}

const char *clientVersionName(ClientVersion version)
{
    switch (version)
    {
    case ClientVersion::ClientVersion_SAMP_037:
        return "SA-MP 0.3.7";
    case ClientVersion::ClientVersion_SAMP_03DL:
        return "SA-MP 0.3.DL";
    case ClientVersion::ClientVersion_openmp:
        return "open.mp";
    }
    return "unknown";
}
} // namespace

AntiCheatSystem::AntiCheatSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_antiCheatService(serviceRegister.getService<AntiCheatService>()),
      m_adminService(serviceRegister.getService<AdminService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_timerService(serviceRegister.getService<TimerService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    // Дефолты весов/порога — до первой записи: сервис создаётся раньше систем и
    // сам таблицу не заполняет.
    m_antiCheatService.resetTuning();

    // Агрессивный режим: пока онлайн нет ни одного залогиненного админа, счёт
    // копится быстрее — за игроками некому смотреть вживую. Считаем в момент
    // нарушения, а не по таймеру: события редкие, а поллинг дал бы окно неточности
    // ровно на входе/выходе админа.
    m_antiCheatService.setAdminPresenceCheck([this]() { return anyAdminOnline(); });

    m_sessionService.subscribeStart([this](IPlayer &player, const PlayerSessionService::Session &session)
                                    { loadProfile(player, session); });

    m_antiCheatService.subscribe(
        [this](int playerId, AntiCheatService::ViolationType type, const AntiCheatService::PlayerRecord &record)
        { onViolation(playerId, type, record); });
}

void AntiCheatSystem::onViolation(int playerId, AntiCheatService::ViolationType type,
                                  const AntiCheatService::PlayerRecord &record)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }

    LogManager::log(LogLevel::Warning, fmt::format("[AntiCheat] {} (id {}): {} — {}", player->getName(), playerId,
                                                   AntiCheatService::name(type), record.recent.back().detail));

    notifyAdmins(*player, type);

    if (record.score >= m_antiCheatService.threshold() && !m_kickPending[playerId])
    {
        LogManager::log(LogLevel::Warning,
                        fmt::format("[AntiCheat] Kicking {} (id {}): score {:.2f}/{:.2f} (multiplier {:.2f}), "
                                    "{} violations total",
                                    player->getName(), playerId, record.score, m_antiCheatService.threshold(),
                                    record.multiplier, record.total));
        kickWithNotice(*player, record);
    }
}

void AntiCheatSystem::kickWithNotice(IPlayer &player, const AntiCheatService::PlayerRecord &record)
{
    const int playerId = player.getID();
    m_kickPending[playerId] = true;

    // Личный множитель пойманного — в БД: следующая его сессия начнётся с меньшей
    // терпимостью. Пишем сразу, а не в отложенном колбэке кика: до срабатывания
    // таймера игрок может отвалиться сам, и запись бы потерялась.
    raiseMultiplier(player);

    // Что именно сработало — в человеческих формулировках и БЕЗ порогов и цифр
    // детекта: игроку хватает категории, а читеру знать, на чём он спалился и
    // насколько промахнулся, незачем. Дубли типов не повторяем.
    std::vector<AntiCheatService::ViolationType> shown;
    std::string reasons;
    for (const AntiCheatService::Violation &violation : record.recent)
    {
        if (std::find(shown.begin(), shown.end(), violation.type) != shown.end())
            continue;
        shown.push_back(violation.type);
        if (!reasons.empty())
            reasons += ", ";
        reasons += reasonText(violation.type);
    }

    // Формулировка ОБЩАЯ и одинаковая всегда: игрок должен знать про саму механику,
    // но сообщать ему, что прямо сейчас админов нет, нельзя — это подсказка, когда
    // сервер без присмотра.
    const char *aggressionNote = "Когда администрации нет в сети, защита реагирует строже обычного.";

    const std::string body =
        fmt::format("Защита сервера отключает вас от игры.\n\n"
                    "Что зафиксировано: {}\n"
                    "Событий за сессию: {}\n\n"
                    "Так выглядит работа стороннего ПО или модификаций игры.\n"
                    "{}\n\n"
                    "Если вы уверены, что это ошибка, напишите администрации:\n"
                    "укажите время и что именно делали в этот момент.",
                    reasons.empty() ? "недопустимые действия клиента" : reasons, record.total, aggressionNote);

    m_dialogService.show(player, makeDialog(DialogStyle_MSGBOX, "Отключение от сервера", body, "Понятно", ""),
                         [](DialogResponse, int, StringView) {});

    // Пауза перед разрывом: диалог должен успеть дойти и быть прочитанным. Тянуть
    // дольше нельзя — всё это время игрок остаётся в игре.
    m_timerService.setTimeout(KICK_DELAY,
                              [this, playerId]()
                              {
                                  if (IPlayer *target = m_core.getPlayers().get(playerId))
                                      target->kick();
                              });
}

void AntiCheatSystem::loadProfile(IPlayer &player, const PlayerSessionService::Session &session)
{
    if (session.accountId == PlayerSessionService::NO_ACCOUNT)
        return; // без аккаунта множитель хранить негде — остаётся дефолтный

    DatabaseManager::selectQuery<float>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult result = schema.getTable("anticheat_profile")
                                           .select("multiplier")
                                           .where("account_id = :account")
                                           .limit(1)
                                           .bind("account", accountId)
                                           .execute();
            if (mysqlx::Row row = result.fetchOne())
                return static_cast<float>(row.get(0).get<double>());
            return 1.0f; // нет строки — анти-чит этого игрока ещё не ловил
        },
        [this, playerId = player.getID(), serial = session.serial](float multiplier)
        {
            // Serial-guard: пока шёл запрос, в слоте могла смениться сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            m_antiCheatService.setMultiplier(playerId, multiplier);
        },
        [](const std::string &error)
        { LogManager::log(Error, "AntiCheatSystem: failed to load anticheat profile: " + error); });
}

void AntiCheatSystem::raiseMultiplier(IPlayer &player)
{
    const int playerId = player.getID();
    const float raised = m_antiCheatService.kickedMultiplier();
    if (m_antiCheatService.multiplier(playerId) >= raised)
        return; // уже не ниже — второй раз поднимать нечего

    m_antiCheatService.setMultiplier(playerId, raised);

    const PlayerSessionService::AccountId accountId = m_sessionService.getAccountId(playerId);
    if (accountId == PlayerSessionService::NO_ACCOUNT)
        return; // гость: множитель живёт только до конца сессии

    DatabaseManager::throwQuery(
        [accountId, raised](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO anticheat_profile (account_id, multiplier, kicks, updated_at) "
                     "VALUES (?, ?, 1, NOW()) "
                     "ON DUPLICATE KEY UPDATE multiplier = GREATEST(multiplier, VALUES(multiplier)), "
                     "kicks = kicks + 1, updated_at = VALUES(updated_at)")
                .bind(accountId, static_cast<double>(raised))
                .execute();
        },
        [](const std::string &error)
        { LogManager::log(Error, "AntiCheatSystem: failed to persist anticheat profile: " + error); });
}

bool AntiCheatSystem::anyAdminOnline() const
{
    // Считаем только доказавших пароль: незалогиненный админ прав не имеет и
    // отреагировать на нарушение не может — для анти-чита его как бы нет.
    for (IPlayer *player : m_core.getPlayers().entries())
    {
        if (player && m_adminService.isLoggedIn(player->getID()))
            return true;
    }
    return false;
}

void AntiCheatSystem::notifyAdmins(IPlayer &player, AntiCheatService::ViolationType type) const
{
    // Ник приходит от клиента: цветокоды в нём клиент отрендерит и перебьёт строку —
    // обезвреживаем. Через u() строку НЕ гоняем: ник уже в кодировке клиента, а сама
    // обвязка латинская.
    const StringView name = player.getName();
    const std::string line =
        fmt::format("[AC] {}[{}] ({})", Encoding::neutralizeColorCodes(std::string_view(name.data(), name.size())),
                    player.getID(), AntiCheatService::name(type));

    // O(online) на нарушение; сами детекторы пишут не чаще раза в 2 с на игрока,
    // так что поток сообщений ограничен ими.
    for (IPlayer *other : m_core.getPlayers().entries())
    {
        if (other && m_adminService.isLoggedIn(other->getID()))
        {
            other->sendClientMessage(AC_COLOUR, line);
        }
    }
}

void AntiCheatSystem::onPlayerConnect(IPlayer &player)
{
    // Версию заявляет клиент, доверять ей нельзя — это телеметрия, не проверка:
    // по ней видно, с каких сборок идут нарушения (0.3.7 — база большинства читов).
    const StringView build = player.getClientVersionName();
    LogManager::log(LogLevel::Message,
                    fmt::format("[AntiCheat] {} (id {}) connected: client {} (build \"{}\")", player.getName(),
                                player.getID(), clientVersionName(player.getClientVersion()),
                                std::string(build.data(), build.size())));
}

void AntiCheatSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_antiCheatService.clear(player.getID());
    m_kickPending[player.getID()] = false; // слот переиспользуется — чужой кик не наследуем
}
