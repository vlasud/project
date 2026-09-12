#include "Systems/ReturnPointSystem/ReturnPointSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/Sanitize.h"
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <mysqlx/xdevapi.h>
#include <optional>

namespace
{
const Colour BUBBLE_COLOUR{255, 255, 255};
const Colour ERROR_COLOUR{255, 90, 90};
constexpr Milliseconds BUBBLE_TIME{5000};
// Срок годности предложения. Колбэк диалога живёт до ответа или дисконнекта, сам не
// истекает: без срока клиент придержал бы ответ и телепортировался посреди боя.
constexpr Seconds PROMPT_TTL{60};
// Насколько близко к сохранённой точке спавн считается «тем же местом»:
// предлагать вернуться туда, где игрок и так стоит, незачем. Сверяется ВМЕСТЕ с
// интерьером и виртуальным миром — одни и те же координаты в разных мирах это
// разные комнаты (у бизнесов и домов интерьеры делятся между владельцами).
constexpr float SAME_PLACE_DISTANCE = 8.0f;
constexpr double ANGLE_LIMIT = 3600.0; // за этим — мусор в строке, а не разворот

// Годная мировая координата из БД; nullopt — значение негодное. Проверка идёт ДО
// сужения: приведение double, не влезающего во float, — UB, а битая строка не
// должна ни ронять воркер, ни утечь в телепорт.
std::optional<float> worldCoord(double value)
{
    if (!std::isfinite(value) || std::fabs(value) > static_cast<double>(ReturnPointService::WORLD_LIMIT))
        return std::nullopt;
    return static_cast<float>(value);
}

// Угол — косметика разворота: мусор трактуем как 0, точку из-за него не теряем.
float safeAngle(double value)
{
    if (!std::isfinite(value) || std::fabs(value) > ANGLE_LIMIT)
        return 0.0f;
    return static_cast<float>(value);
}
} // namespace

ReturnPointSystem::ReturnPointSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_returnService(serviceRegister.getService<ReturnPointService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_healthService(serviceRegister.getService<PlayerHealthService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_bubbleService(serviceRegister.getService<PlayerBubbleService>()),
      m_cameraService(serviceRegister.getService<CameraService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
    listen(core.getPlayers().getPlayerSpawnDispatcher(), this);

    m_sessionService.subscribeStart([this](IPlayer &player, const PlayerSessionService::Session &session)
                                    { loadPoint(player, session); });
    // Персист в save-канал: идемпотентный UPSERT снимка. Зовётся и на конце сессии
    // (внутри end, до teardown), и периодически автосейвом.
    m_sessionService.subscribeSave([this](IPlayer &player, const PlayerSessionService::Session &session)
                                   { persistPoint(player, session); });
    // Конец сессии — teardown слота: флаги и точка принадлежат РОВНО одной сессии, а
    // start умеет открыть новую сессию на том же подключении (смена аккаунта без
    // дисконнекта). Порядок каналов (save -> end) снимок не ломает.
    m_sessionService.subscribeEnd([this](IPlayer &player, const PlayerSessionService::Session &)
                                  { m_returnService.reset(player.getID()); });
}

void ReturnPointSystem::onPlayerConnect(IPlayer &player)
{
    m_returnService.reset(player.getID());
}

void ReturnPointSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_returnService.reset(player.getID());
}

void ReturnPointSystem::onPlayerSpawn(IPlayer &player)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
        return; // спавн до логина (класс-селекшн, спектейт авторизации) — не наш случай
    if (m_returnService.isSpawned(playerId))
        return; // респавн после смерти: возврат предлагается только на логин-спавне

    // С этого момента принятая позиция — реальное место игрока: снимок разрешён.
    // Момент спавна запоминается — от него течёт срок предложения вернуться.
    m_returnService.markSpawned(playerId, Time::now());
    offerReturn(player, *session);
}

// ------------------------------------------------------------------ загрузка по сессии

void ReturnPointSystem::loadPoint(IPlayer &player, const PlayerSessionService::Session &session)
{
    if (session.accountId == PlayerSessionService::NO_ACCOUNT)
        return; // без аккаунта негде хранить

    DatabaseManager::selectQuery<std::optional<ReturnPointService::Point>>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult result = schema.getTable("player_return_point")
                                           .select("x", "y", "z", "angle", "interior", "virtual_world")
                                           .where("account_id = :account")
                                           .limit(1)
                                           .bind("account", accountId)
                                           .execute();
            // Нет строки — точки возврата у аккаунта нет.
            std::optional<ReturnPointService::Point> point;
            if (mysqlx::Row row = result.fetchOne())
            {
                // Числа читаем широкими типами (double/int64) и сужаем сами: битый
                // ряд не должен ронять get<int>() исключением на воркере (как
                // InventorySystem::loadItems), а мусорное значение — доехать до
                // телепорта. Любая непрошедшая проверку часть -> точки нет.
                const std::optional<float> x = worldCoord(row.get(0).get<double>());
                const std::optional<float> y = worldCoord(row.get(1).get<double>());
                const std::optional<float> z = worldCoord(row.get(2).get<double>());
                const std::int64_t interior = row.get(4).get<std::int64_t>();
                const std::int64_t virtualWorld = row.get(5).get<std::int64_t>();
                // Диапазоны здесь — только безопасность сужения к unsigned/int;
                // годность точки решает ReturnPointService::isValid в load().
                if (x && y && z && interior >= 0 && interior <= ReturnPointService::MAX_INTERIOR &&
                    virtualWorld >= 0 && virtualWorld <= std::numeric_limits<int>::max())
                {
                    ReturnPointService::Point loaded;
                    loaded.position = Vector3(*x, *y, *z);
                    loaded.angle = safeAngle(row.get(3).get<double>());
                    loaded.interior = static_cast<unsigned>(interior);
                    loaded.virtualWorld = static_cast<int>(virtualWorld);
                    point = loaded;
                }
            }
            return point;
        },
        [this, playerId = player.getID(), serial = session.serial](std::optional<ReturnPointService::Point> point)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/другая сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            IPlayer *target = m_core.getPlayers().get(playerId);
            if (!target)
                return;
            m_returnService.load(playerId, point);
            // Логин-спавн мог пройти раньше загрузки — тогда предложение здесь.
            offerReturn(*target, *current);
        },
        [](const std::string &error)
        { LogManager::log(Error, "ReturnPointSystem: failed to load return point: " + error); });
}

// ------------------------------------------------------------------ сохранение

void ReturnPointSystem::persistPoint(IPlayer &player, const PlayerSessionService::Session &session)
{
    if (session.accountId == PlayerSessionService::NO_ACCOUNT)
        return;
    const int playerId = player.getID();

    // Гейт — пройденный логин-спавн: до него игрок висит в спектейте авторизации, и
    // снимок этой «позиции» затёр бы в БД реальную точку аккаунта, оборвись сессия
    // до спавна (дисконнект в спектейте, автосейв в том же окне).
    if (!m_returnService.isSpawned(playerId))
        return;
    // И трек позиции обязан быть живым. Сброшенный слот локации отдаёт нулевой
    // вектор, а он — правдоподобная точка мира (океан под картой): такой снимок
    // прошёл бы валидацию и затёр точку аккаунта. Проверяем факт, а не порядок
    // обработчиков дисконнекта (сброс локации может оказаться раньше нашего save).
    if (!m_locationService.hasPosition(playerId))
        return;

    // Серверные факты: принятая позиция/интерьер/мир из PlayerLocationService (не
    // сырые клиентские). Угол — из поворота модели: поворот в источник правды о
    // местонахождении не входит; у игрока в транспорте он не обновляется (позицию
    // пишет vehicle-синк, поворот — только пеший), поэтому за рулём в снимок уходит
    // последний пеший курс. Транспорт не сохраняем: вышел за рулём — вернётся пешком
    // на те же координаты.
    ReturnPointService::Point snapshot;
    snapshot.position = m_locationService.getPosition(playerId);
    snapshot.angle = Utils::finiteOrZero(player.getRotation().ToEuler().z);
    snapshot.interior = m_locationService.getInterior(playerId);
    snapshot.virtualWorld = m_locationService.getVirtualWorld(playerId);
    // Пишем не шире, чем читаем: снимок, который загрузка сочтёт битым, не пишем
    // вовсе — иначе точка аккаунта молча заменилась бы на «нет точки».
    if (!ReturnPointService::isValid(snapshot))
        return;

    // Упорядочено по аккаунту — тот же двойной save-канал, что у денег и оружия
    // (конец сессии и автосейв способны оказаться в полёте одновременно, и более
    // старый снимок не должен лечь последним).
    DatabaseManager::throwQueryOrdered(
        fmt::format("player_return_point:{}", session.accountId),
        [accountId = session.accountId, snapshot](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO player_return_point (account_id, x, y, z, angle, interior, virtual_world) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?) "
                     "ON DUPLICATE KEY UPDATE x = VALUES(x), y = VALUES(y), z = VALUES(z), "
                     "angle = VALUES(angle), interior = VALUES(interior), virtual_world = VALUES(virtual_world)")
                .bind(accountId, static_cast<double>(snapshot.position.x), static_cast<double>(snapshot.position.y),
                      static_cast<double>(snapshot.position.z), static_cast<double>(snapshot.angle),
                      static_cast<int>(snapshot.interior), snapshot.virtualWorld)
                .execute();
        },
        [accountId = session.accountId](const std::string &error)
        {
            LogManager::log(Error, fmt::format("ReturnPointSystem: failed to persist return point of "
                                               "account {}: {}",
                                               accountId, error));
        });
}

// ------------------------------------------------------------------ предложение вернуться

void ReturnPointSystem::offerReturn(IPlayer &player, const PlayerSessionService::Session &session)
{
    const int playerId = player.getID();
    if (!m_returnService.isSpawned(playerId) || !m_returnService.isLoaded(playerId))
        return; // ждём вторую половину гонки: логин-спавн или загрузку из БД
    if (m_returnService.isPromptDone(playerId))
        return; // предложение делается один раз за сессию
    if (!m_returnService.getPoint(playerId))
        return; // новый аккаунт или битая строка — возвращать некуда
    if (Time::now() > m_returnService.spawnedAt(playerId) + PROMPT_TTL)
    {
        // Загрузка сильно опоздала (очередь пула БД на массовом логине): предложение
        // с уже вышедшим сроком не показываем — принять его всё равно нельзя.
        m_returnService.markPromptDone(playerId);
        return;
    }
    if (spawnedAtSamePlace(playerId))
    {
        // Спавн совпал с сохранённой точкой — возвращать некуда.
        m_returnService.markPromptDone(playerId);
        return;
    }

    showDialog(player, session);
}

bool ReturnPointSystem::spawnedAtSamePlace(int playerId) const
{
    const ReturnPointService::Point *point = m_returnService.getPoint(playerId);
    if (!point)
        return false;
    // Мир и интерьер — часть адреса, а не декорация: те же x/y/z в другом
    // виртуальном мире это другая комната, и туда вернуться есть смысл.
    if (m_locationService.getInterior(playerId) != point->interior ||
        m_locationService.getVirtualWorld(playerId) != point->virtualWorld)
        return false;
    return glm::distance(m_locationService.getPosition(playerId), point->position) <= SAME_PLACE_DISTANCE;
}

void ReturnPointSystem::showDialog(IPlayer &player, const PlayerSessionService::Session &session)
{
    m_returnService.markPromptDone(player.getID());

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_MSGBOX, "Возвращение",
                   "В прошлый раз вы вышли из игры в другом месте.\n\n"
                   "Вернуться туда или остаться здесь?",
                   "Вернуться", "Остаться"),
        [this, playerId = player.getID(), serial = session.serial,
         deadline = m_returnService.spawnedAt(player.getID()) + PROMPT_TTL](DialogResponse response, int, StringView)
        {
            if (response != DialogResponse_Left)
                return; // «Остаться» или закрытие — ничего не делаем

            // К моменту ответа игрок мог отключиться, перезайти (другая сессия в том
            // же слоте), умереть или сесть в машину.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            IPlayer *target = m_core.getPlayers().get(playerId);
            if (!target)
                return;

            // Просроченный ответ: предложение действует только сразу после входа.
            // Срок привязан к ЛОГИН-СПАВНУ, не к показу диалога: при занятом пуле БД
            // загрузка точки уходит в очередь, и диалог может всплыть, когда игрок
            // уже уехал — от показа срок дал бы возврат из произвольной точки.
            if (Time::now() > deadline)
            {
                target->sendClientMessage(ERROR_COLOUR, u("Предложение вернуться на прошлое место больше не "
                                                          "действует"));
                return;
            }
            if (!m_healthService.isAlive(playerId))
            {
                // Мёртвого не тянем: респавн сам уведёт его в точку спавна.
                target->sendClientMessage(ERROR_COLOUR, u("Сейчас нельзя вернуться на прошлое место"));
                return;
            }

            // Стейт серверный, клиенту не верим. Серверный телепорт двигает только
            // игрока (ядро на setPosition делает resetVehicle — машина осталась бы
            // позади), поэтому за рулём/пассажиром просим выйти (как /tp в GpsSystem).
            const PlayerState state = m_stateService.getState(playerId);
            // Посадка и высадка — тот же случай, что «за рулём»: игрок привязан
            // к машине, и телепорт оставил бы её позади.
            if (state == PlayerState_Driver || state == PlayerState_Passenger ||
                state == PlayerState_EnterVehicleDriver || state == PlayerState_EnterVehiclePassenger ||
                state == PlayerState_ExitVehicle)
            {
                target->sendClientMessage(ERROR_COLOUR, u("Выйдите из транспорта, чтобы вернуться на прошлое место"));
                return;
            }
            if (state != PlayerState_OnFoot)
            {
                target->sendClientMessage(ERROR_COLOUR, u("Сейчас нельзя вернуться на прошлое место"));
                return;
            }

            returnToPoint(*target);
        });
}

void ReturnPointSystem::returnToPoint(IPlayer &player)
{
    const ReturnPointService::Point *stored = m_returnService.getPoint(player.getID());
    if (!stored)
        return;
    const ReturnPointService::Point point = *stored; // копия: не держим указатель в слот через вызовы

    // Серверный телепорт с грейсом анти-чита (позиция + интерьер + мир + разворот),
    // НЕ сырой setPosition. Точку спавна не трогаем — это разовый перенос.
    m_locationService.teleport(player, point.position, point.interior, point.virtualWorld, point.angle);
    m_cameraService.setBehind(player); // иначе камера осталась бы смотреть прежним курсом
    m_bubbleService.show(player, u("Вернулся"), BUBBLE_COLOUR, BUBBLE_TIME);
}
