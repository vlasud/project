#include "Systems/PhoneSystem/PhoneSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Systems/Factions/PoliceLasVenturasSystem/PoliceLasVenturasSystem.h"
#include "Systems/Factions/PoliceLosSantosSystem/PoliceLosSantosSystem.h"
#include "Systems/Factions/PoliceSanFierroSystem/PoliceSanFierroSystem.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <random>
#include <string>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};
// Вызов службы — отдельный канал: заметнее обычных информационных строк.
const Colour CALL_COLOUR{255, 200, 80};

// Звук уведомления о вызове (id от владельца).
constexpr std::uint32_t CALL_SOUND = 17001;

// Сколько попыток подобрать свободный номер телефона при коллизии.
constexpr int PHONE_ISSUE_ATTEMPTS = 8;

std::string safeName(IPlayer &player)
{
    const StringView name = player.getName();
    return Encoding::neutralizeColorCodes(std::string_view(name.data(), name.size()));
}

TimePoint now()
{
    return std::chrono::steady_clock::now();
}

bool isPoliceFaction(int factionId)
{
    return factionId == PoliceLosSantosSystem::FACTION_ID || factionId == PoliceSanFierroSystem::FACTION_ID ||
           factionId == PoliceLasVenturasSystem::FACTION_ID;
}
} // namespace

PhoneSystem::PhoneSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_phoneService(serviceRegister.getService<PhoneService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_healthService(serviceRegister.getService<PlayerHealthService>()),
      m_factionService(serviceRegister.getService<FactionService>()),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_audioService(serviceRegister.getService<AudioService>())
{
    // Номер телефона аккаунта: грузим на старте сессии, выдаём при первом входе.
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            loadPhone(player, session);
        });
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            onSessionEnd(player);
        });
    // Погиб звонящий — вызов больше не актуален (иначе служба поедет к трупу).
    m_healthService.subscribeDeath(
        [this](IPlayer &player)
        {
            m_phoneService.cancelOrdersOf(player.getID());
        });

    // Полицию регистрирует ТЕЛЕФОН, а не своя система: полицейских систем несколько
    // (LS/SF/LV), общей «работы» у них пока нет. Когда полицию доработают, эта
    // регистрация переедет к ней — телефон менять не придётся.
    m_phoneService.registerDispatch(PhoneService::Service::Police, "полиции",
                                    [this](const PhoneService::WorkerVisitor &visit)
                                    {
                                        for (IPlayer *officer : m_core.getPlayers().entries())
                                        {
                                            if (!officer)
                                            {
                                                continue;
                                            }
                                            const int officerId = officer->getID();
                                            if (m_sessionService.isActive(officerId) &&
                                                isPoliceFaction(m_factionService.getMemberFaction(officerId)))
                                            {
                                                visit(officerId);
                                            }
                                        }
                                    });

    PlayerCommandService &commands = serviceRegister.getService<PlayerCommandService>();
    // /c без параметра открывает меню; с параметром — сразу набор номера. Параметр
    // объявлен НЕОБЯЗАТЕЛЬНЫМ: иначе разбор отверг бы голое /c подсказкой usage.
    commands.add(
        "c", {{PlayerCommandService::Param::String, "номер телефона", /*optional=*/true}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            const StringView entered = args.count() > 0 ? args.getString(0) : StringView{};
            onCallCommand(player, std::string(entered.data(), entered.size()));
        },
        {}, "позвонить: службы или номер игрока", PlayerCommandService::HelpCategory::Misc);

    commands.add(
        "acceptjob", {{PlayerCommandService::Param::Int, "номер вызова"}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            onAcceptJobCommand(player, args.getInt(0));
        },
        {}, "принять вызов по номеру (для работников служб)", PlayerCommandService::HelpCategory::Economy);
}

// ------------------------------------------------------------------ команды

void PhoneSystem::onCallCommand(IPlayer &player, const std::string &argument)
{
    if (argument.empty())
    {
        showCallMenu(player);
        return;
    }
    // Быстрый набор: /c <номер> — то же, что пункт «Ввести номер телефона».
    char *end = nullptr;
    const long long parsed = std::strtoll(argument.c_str(), &end, 10);
    if (end == argument.c_str() || *end != '\0')
    {
        player.sendClientMessage(ERROR_COLOUR, u("Номер телефона — только цифры. Без номера: /c"));
        return;
    }
    dialNumber(player, static_cast<std::int64_t>(parsed));
}

void PhoneSystem::showCallMenu(IPlayer &player)
{
    const int playerId = player.getID();
    const std::int64_t own = m_phoneService.phoneOf(playerId);

    const std::string title =
        own == PhoneService::NO_PHONE ? std::string("Телефон") : fmt::format("Телефон — ваш номер {}", own);
    const std::string body = "Скорая\nПолиция\nТакси\nВвести номер телефона";

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, title, body, "Позвонить", "Закрыть"),
                         [this, playerId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *caller = m_core.getPlayers().get(playerId);
                             if (!caller || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             switch (listItem)
                             {
                             case 0:
                                 placeServiceCall(*caller, PhoneService::Service::Ambulance);
                                 break;
                             case 1:
                                 placeServiceCall(*caller, PhoneService::Service::Police);
                                 break;
                             case 2:
                                 placeServiceCall(*caller, PhoneService::Service::Taxi);
                                 break;
                             case 3:
                                 showDialNumberDialog(*caller);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void PhoneSystem::showDialNumberDialog(IPlayer &player)
{
    const int playerId = player.getID();
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Набор номера", "Введите номер телефона игрока, которому хотите позвонить",
                   "Позвонить", "Назад"),
        [this, playerId](DialogResponse response, int, StringView text)
        {
            IPlayer *caller = m_core.getPlayers().get(playerId);
            if (!caller)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showCallMenu(*caller);
                return;
            }
            const std::string entered(text.data(), text.size());
            char *end = nullptr;
            const long long parsed = std::strtoll(entered.c_str(), &end, 10);
            if (entered.empty() || end == entered.c_str() || *end != '\0')
            {
                caller->sendClientMessage(ERROR_COLOUR, u("Номер телефона — только цифры"));
                return;
            }
            dialNumber(*caller, static_cast<std::int64_t>(parsed));
        });
}

// ------------------------------------------------------------------ вызовы служб

void PhoneSystem::placeServiceCall(IPlayer &player, PhoneService::Service service)
{
    const int callerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(callerId);
    if (!session)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы звонить"));
        return;
    }
    if (!m_phoneService.hasDispatch(service))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Эта служба сейчас недоступна"));
        return;
    }

    const TimePoint timeNow = now();
    // Позиция звонка — ПРИНЯТАЯ сервером, а не заявление клиента: по ней поедет работник.
    const Vector3 position = m_locationService.getPosition(callerId);
    const int number = m_phoneService.createOrder(service, callerId, session->serial, position, timeNow);
    if (number == 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Ваш предыдущий вызов ещё в работе — дождитесь ответа"));
        return;
    }

    const std::string callName = m_phoneService.callNameOf(service);
    const std::string callerName = safeName(player);

    int notified = 0;
    m_phoneService.forEachWorker(
        service,
        [&](int workerId)
        {
            // Звонящего НЕ пропускаем: вызов идёт всем, кто на смене, и работник этой
            // же службы среди них. Он и увидит свой заказ, и сможет его принять —
            // отдельного запрета тут нет (в одиночку это единственный способ вообще
            // проверить, что вызовы доходят).
            IPlayer *worker = m_core.getPlayers().get(workerId);
            if (!worker)
            {
                return;
            }
            // Дистанция у каждого СВОЯ — от него до места вызова.
            const Vector3 delta = m_locationService.getPosition(workerId) - position;
            const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            worker->sendClientMessage(CALL_COLOUR,
                                      u(fmt::format("Вызов {} номер {}. {}[{}]. Расстояние от вас {:.0f} метров",
                                                    callName, number, callerName, callerId, distance)));
            worker->sendClientMessage(CALL_COLOUR, u(fmt::format("Принять: /acceptjob {}", number)));
            m_audioService.playSound(*worker, CALL_SOUND);
            ++notified;
        });

    if (notified == 0)
    {
        // Заказ не держим: некому его принять, а звонящий иначе не смог бы позвонить снова.
        m_phoneService.cancelOrdersOf(callerId);
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Никто из {} сейчас не на смене — вызывать некого", callName)));
        return;
    }

    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Вызов {} принят в обработку, номер {}. Ожидайте ответа", callName,
                                           number)));
}

void PhoneSystem::onAcceptJobCommand(IPlayer &worker, int orderNumber)
{
    const int workerId = worker.getID();

    // Какую службу представляет принимающий — решает его собственная занятость:
    // перебираем зарегистрированные и ищем ту, где он числится на смене.
    PhoneService::Service own = PhoneService::Service::Count;
    for (int i = 0; i < static_cast<int>(PhoneService::Service::Count); ++i)
    {
        const auto service = static_cast<PhoneService::Service>(i);
        bool onDuty = false;
        m_phoneService.forEachWorker(service,
                                     [&](int id)
                                     {
                                         if (id == workerId)
                                         {
                                             onDuty = true;
                                         }
                                     });
        if (onDuty)
        {
            own = service;
            break;
        }
    }
    if (own == PhoneService::Service::Count)
    {
        worker.sendClientMessage(ERROR_COLOUR, u("Принимать вызовы может только работник службы на смене"));
        return;
    }

    PhoneService::Order order;
    if (!m_phoneService.acceptOrder(orderNumber, own, now(), order))
    {
        worker.sendClientMessage(ERROR_COLOUR, u("Такого вызова нет — его уже приняли или он истёк"));
        return;
    }

    // Звонящий мог выйти, а слот — достаться другому игроку: сверяем сессию.
    IPlayer *caller = m_core.getPlayers().get(order.callerId);
    const PlayerSessionService::Session *callerSession = m_sessionService.get(order.callerId);
    if (!caller || !callerSession || callerSession->serial != order.callerSerial)
    {
        worker.sendClientMessage(ERROR_COLOUR, u("Звонивший уже не в игре — вызов отменён"));
        return;
    }

    // Указатель на место вызова — через единого владельца чекпоинт-слота.
    m_waypointService.showFor(worker, order.position);
    worker.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Вызов {} принят. Место отмечено красным чекпоинтом", order.number)));
    caller->sendClientMessage(INFO_COLOUR,
                              u(fmt::format("Ваш вызов {} принят: {}[{}] уже едет к вам", order.number,
                                            safeName(worker), workerId)));
}

void PhoneSystem::dialNumber(IPlayer &player, std::int64_t phone)
{
    if (phone < PhoneService::PHONE_MIN || phone > PhoneService::PHONE_MAX)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Номера телефонов — от {} до {}", PhoneService::PHONE_MIN,
                                               PhoneService::PHONE_MAX)));
        return;
    }
    // Телефонная связь между игроками — следующий этап: номера уже выданы и
    // резолвятся, но разговора пока нет.
    player.sendClientMessage(ERROR_COLOUR, u("Звонки игрокам пока не работают — появятся позже"));
}

// ------------------------------------------------------------------ лайфцикл сессии

void PhoneSystem::loadPhone(IPlayer &player, const PlayerSessionService::Session &session)
{
    // Загрузка И выдача — одним запросом на воркере: номер должен быть уникальным, а
    // проверять уникальность по кругу с главного потока значило бы гонять несколько
    // async-запросов подряд. Коллизию ловим по UNIQUE-индексу и пробуем другой номер.
    DatabaseManager::selectQuery<std::int64_t>(
        [accountId = session.accountId](mysqlx::Schema schema) -> std::int64_t
        {
            mysqlx::RowResult result = schema.getTable("player")
                                           .select("phone")
                                           .where("id = :id")
                                           .limit(1)
                                           .bind("id", accountId)
                                           .execute();
            std::int64_t phone = PhoneService::NO_PHONE;
            if (mysqlx::Row row = result.fetchOne())
            {
                if (!row.get(0).isNull())
                {
                    phone = row.get(0).get<std::int64_t>();
                }
            }
            if (phone != PhoneService::NO_PHONE)
            {
                return phone;
            }

            // Номера ещё нет — выдаём. Пишем условно (phone IS NULL OR phone = 0),
            // чтобы параллельная выдача тому же аккаунту не перетёрла уже выданный.
            std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<std::int64_t> dist(PhoneService::PHONE_MIN, PhoneService::PHONE_MAX);
            for (int attempt = 0; attempt < PHONE_ISSUE_ATTEMPTS; ++attempt)
            {
                const std::int64_t candidate = dist(rng);
                try
                {
                    schema.getSession()
                        .sql("UPDATE player SET phone = ? WHERE id = ? AND (phone IS NULL OR phone = 0)")
                        .bind(candidate, accountId)
                        .execute();
                }
                catch (const mysqlx::Error &)
                {
                    continue; // номер занят другим аккаунтом (UNIQUE) — берём следующий
                }
                mysqlx::RowResult check = schema.getTable("player")
                                              .select("phone")
                                              .where("id = :id")
                                              .limit(1)
                                              .bind("id", accountId)
                                              .execute();
                if (mysqlx::Row row = check.fetchOne())
                {
                    if (!row.get(0).isNull())
                    {
                        return row.get(0).get<std::int64_t>();
                    }
                }
            }
            return PhoneService::NO_PHONE;
        },
        [this, playerId = player.getID(), serial = session.serial](std::int64_t phone)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
            {
                return;
            }
            if (phone == PhoneService::NO_PHONE)
            {
                LogManager::log(Error, "PhoneSystem: failed to issue a phone number");
                return;
            }
            m_phoneService.setPhone(playerId, phone);
            if (IPlayer *player = m_core.getPlayers().get(playerId))
            {
                player->sendClientMessage(INFO_COLOUR,
                                          u(fmt::format("Ваш номер телефона: {}. Позвонить — /c", phone)));
            }
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "PhoneSystem: failed to load phone number: " + error);
        });
}

void PhoneSystem::onSessionEnd(IPlayer &player)
{
    // Слот переиспользуется — чужой номер и чужие вызовы не наследуем.
    m_phoneService.resetPlayer(player.getID());
}
