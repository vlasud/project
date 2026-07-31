#include "Systems/FriendSystem/FriendSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <string_view>
#include <utility>

namespace
{
const Colour INFO_COLOUR{120, 220, 255}; // голубой: им же приходит «друг зашёл в игру»
const Colour ERROR_COLOUR{255, 90, 90};

// Цветокоды ВНУТРИ строки заявки: обе команды подсвечены прямо в тексте, чтобы их
// было видно в потоке чата. Это НАШИ коды, а не игроцкие — ник подставляется уже
// обезвреженным.
constexpr const char *ACCEPT_COLOUR_CODE = "{00C853}"; // зелёный
constexpr const char *CANCEL_COLOUR_CODE = "{FF8C00}"; // оранжевый

TimePoint now()
{
    return std::chrono::steady_clock::now();
}

// Ник игрока для подстановки в чат: цветокоды в нём интерпретирует клиент, поэтому
// чужой ник обязан пройти нейтрализацию.
std::string safeName(IPlayer &player)
{
    const StringView name = player.getName();
    return Encoding::neutralizeColorCodes(std::string_view(name.data(), name.size()));
}

// Имя из БД в диалог: там оно тоже игроцкое.
std::string safeName(const std::string &name)
{
    return Encoding::neutralizeColorCodes(name);
}
} // namespace

FriendSystem::FriendSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_friendService(serviceRegister.getService<FriendService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_timers(serviceRegister.getService<TimerService>())
{
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            loadFriends(player, session);
        });
    // Отметка «последний раз в сети» идёт в save-канал: он срабатывает и на конце
    // сессии, и на автосейве, поэтому после краша отметка отстаёт максимум на
    // интервал автосейва, а не теряется целиком.
    m_sessionService.subscribeSave(
        [this](IPlayer &, const PlayerSessionService::Session &session)
        {
            touchLastSeen(session);
        });
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            onSessionEnd(player);
        });

    PlayerCommandService &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add(
        "friend", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            showMenu(player);
        },
        {}, "друзья: список и добавление", PlayerCommandService::HelpCategory::Misc);

    // Заявок может висеть несколько, поэтому отвечаем на КОНКРЕТНУЮ — по id того, кто
    // её прислал. Он есть в самом тексте заявки, вводить наугад ничего не нужно.
    commands.add(
        "friendaccept", {{PlayerCommandService::Param::Int, "id отправителя"}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            onAcceptCommand(player, args.getInt(0));
        },
        {}, "принять заявку в друзья от игрока", PlayerCommandService::HelpCategory::Misc);

    commands.add(
        "friendcancel", {{PlayerCommandService::Param::Int, "id отправителя"}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            onCancelCommand(player, args.getInt(0));
        },
        {}, "отклонить заявку в друзья от игрока", PlayerCommandService::HelpCategory::Misc);
}

void FriendSystem::initialize(IComponentList * /*components*/)
{
    // Один общий тик на все заявки: их единицы, за-игроковых таймеров не плодим.
    // НИКОГДА не отменяется, в том числе из своего колбэка.
    m_requestTimer = m_timers.setInterval(Milliseconds{1000},
                                          [this]()
                                          {
                                              tickRequests();
                                          });
}

// ------------------------------------------------------------------ меню

void FriendSystem::showMenu(IPlayer &player)
{
    const int playerId = player.getID();
    if (!m_sessionService.isActive(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы пользоваться списком друзей"));
        return;
    }

    const std::string body = "Мои друзья\nДобавить в друзья";
    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Друзья", body, "Выбрать", "Закрыть"),
                         [this, playerId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *owner = m_core.getPlayers().get(playerId);
                             if (!owner || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             switch (listItem)
                             {
                             case 0:
                                 showFriendList(*owner);
                                 break;
                             case 1:
                                 showAddDialog(*owner);
                                 break;
                             default:
                                 break;
                             }
                         });
}

// ------------------------------------------------------------------ мои друзья

void FriendSystem::showFriendList(IPlayer &player)
{
    const PlayerSessionService::Session *session = m_sessionService.get(player.getID());
    if (!session)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы пользоваться списком друзей"));
        return;
    }

    // Список тянем ИЗ БД, а не из кэша: «последний раз в сети» у оффлайн-друга иначе
    // застыл бы на моменте нашего входа. Путь холодный — открытие диалога.
    DatabaseManager::selectQuery<std::vector<FriendService::Friend>>(
        [accountId = session->accountId](mysqlx::Schema schema) -> std::vector<FriendService::Friend>
        {
            mysqlx::SqlResult result =
                schema.getSession()
                    .sql("SELECT p.id, p.name, DATE_FORMAT(p.last_seen, '%d.%m.%Y %H:%i') "
                         "FROM player_friend f JOIN player p ON p.id = f.friend_id "
                         "WHERE f.account_id = ? ORDER BY p.name")
                    .bind(accountId)
                    .execute();

            std::vector<FriendService::Friend> friends;
            while (mysqlx::Row row = result.fetchOne())
            {
                FriendService::Friend entry;
                entry.accountId = row.get(0).get<std::int64_t>();
                entry.name = row.get(1).get<std::string>();
                if (!row.get(2).isNull())
                {
                    entry.lastSeen = row.get(2).get<std::string>();
                }
                friends.push_back(std::move(entry));
            }
            return friends;
        },
        [this, playerId = player.getID(), serial = session->serial](std::vector<FriendService::Friend> friends)
        {
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
            {
                return; // в слоте уже другая сессия — чужой список ей не показываем
            }
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }
            // Кэш освежаем тем же снимком: на нём держатся оповещение о входе и
            // проверка «уже в друзьях».
            m_friendService.loadFriends(playerId, friends);
            presentFriendList(*owner, m_friendService.friendsOf(playerId));
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "FriendSystem: failed to load friends: " + error);
        });
}

void FriendSystem::presentFriendList(IPlayer &player, const std::vector<FriendService::Friend> &friends)
{
    const int playerId = player.getID();
    if (friends.empty())
    {
        m_dialogService.show(player,
                             makeDialog(DialogStyle_MSGBOX, "Мои друзья",
                                        "Список друзей пуст.\n\nДобавить друга: /friend -> «Добавить в друзья».",
                                        "Назад", ""),
                             [this, playerId](DialogResponse, int, StringView)
                             {
                                 if (IPlayer *owner = m_core.getPlayers().get(playerId))
                                 {
                                     showMenu(*owner);
                                 }
                             });
        return;
    }

    // ПОРЯДОК СТРОК = порядок friends: listItem приходит от клиента как индекс в нём.
    std::string body;
    for (const FriendService::Friend &entry : friends)
    {
        const int onlineId = m_sessionService.playerByAccount(entry.accountId);
        if (onlineId >= 0)
        {
            body += fmt::format("{}[{}] онлайн\n", safeName(entry.name), onlineId);
        }
        else
        {
            body += fmt::format("{} оффлайн {}\n", safeName(entry.name),
                                entry.lastSeen.empty() ? std::string("(ни разу не выходил)") : entry.lastSeen);
        }
    }
    body.pop_back();

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Мои друзья", body, "Выбрать", "Назад"),
        [this, playerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showMenu(*owner);
                return;
            }
            // Индекс от клиента — сверяем с ЖИВЫМ списком (он мог поменяться, пока
            // висел диалог: друга удалили с другого конца).
            const std::vector<FriendService::Friend> &list = m_friendService.friendsOf(playerId);
            if (listItem < 0 || static_cast<std::size_t>(listItem) >= list.size())
            {
                return;
            }
            const FriendService::Friend &entry = list[static_cast<std::size_t>(listItem)];
            showFriendActions(*owner, entry.accountId, entry.name);
        });
}

void FriendSystem::showFriendActions(IPlayer &player, FriendService::AccountId accountId, const std::string &name)
{
    const int playerId = player.getID();
    m_dialogService.show(player,
                         makeDialog(DialogStyle_LIST, safeName(name), "Удалить из друзей", "Выбрать", "Назад"),
                         [this, playerId, accountId, name](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *owner = m_core.getPlayers().get(playerId);
                             if (!owner)
                             {
                                 return;
                             }
                             if (response != DialogResponse_Left)
                             {
                                 showFriendList(*owner);
                                 return;
                             }
                             if (listItem == 0)
                             {
                                 removeFriend(*owner, accountId, name);
                             }
                         });
}

void FriendSystem::removeFriend(IPlayer &player, FriendService::AccountId accountId, const std::string &name)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session)
    {
        return;
    }
    // Дружбы могло уже не быть: её сняли с другого конца, пока висел диалог.
    if (!m_friendService.removeFriend(playerId, accountId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот игрок уже не у вас в друзьях"));
        return;
    }
    // Вторая сторона: если она онлайн, чиним и её кэш — иначе до перезахода она
    // считала бы дружбу живой и получала оповещения о входе.
    const int friendOnlineId = m_sessionService.playerByAccount(accountId);
    if (friendOnlineId >= 0)
    {
        m_friendService.removeFriend(friendOnlineId, session->accountId);
    }

    // Дружба взаимная — снимаем ОБЕ строки одним DELETE.
    DatabaseManager::throwQuery(
        [own = session->accountId, accountId](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("DELETE FROM player_friend WHERE (account_id = ? AND friend_id = ?) "
                     "OR (account_id = ? AND friend_id = ?)")
                .bind(own, accountId, accountId, own)
                .execute();
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "FriendSystem: failed to delete friendship: " + error);
        });

    player.sendClientMessage(INFO_COLOUR, u(fmt::format("{} удалён из ваших друзей", safeName(name))));
    showFriendList(player);
}

// ------------------------------------------------------------------ добавление

void FriendSystem::showAddDialog(IPlayer &player)
{
    const int playerId = player.getID();
    // Числовой ввод — через обёртку сервиса: мусор и overflow она отсекает сама.
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Добавить в друзья",
                   "Введите id игрока, которого хотите добавить.\nИгрок должен быть в сети.", "Отправить", "Назад"),
        [this, playerId](DialogResponse response, std::int64_t value)
        {
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showMenu(*owner);
                return;
            }
            // id игрока — заведомо маленькое число; всё, что не влезает, отсекаем до
            // сужения к int.
            if (value < 0 || value >= MAX_PLAYERS)
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Игрок с таким id не в сети"));
                return;
            }
            sendRequest(*owner, static_cast<int>(value));
        });
}

void FriendSystem::sendRequest(IPlayer &sender, int targetId)
{
    const int senderId = sender.getID();
    const PlayerSessionService::Session *senderSession = m_sessionService.get(senderId);
    if (!senderSession)
    {
        sender.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы добавлять в друзья"));
        return;
    }
    if (targetId == senderId)
    {
        sender.sendClientMessage(ERROR_COLOUR, u("Себя в друзья не добавить"));
        return;
    }
    if (m_friendService.friendCount(senderId) >= FriendService::MAX_FRIENDS)
    {
        sender.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("У вас уже максимум друзей ({})", FriendService::MAX_FRIENDS)));
        return;
    }

    IPlayer *target = m_core.getPlayers().get(targetId);
    const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
    if (!target || !targetSession)
    {
        sender.sendClientMessage(ERROR_COLOUR, u("Игрок с таким id не в сети"));
        return;
    }
    if (m_friendService.isFriend(senderId, targetSession->accountId))
    {
        sender.sendClientMessage(ERROR_COLOUR, u("Этот игрок уже у вас в друзьях"));
        return;
    }
    if (m_friendService.friendCount(targetId) >= FriendService::MAX_FRIENDS)
    {
        sender.sendClientMessage(ERROR_COLOUR, u("У этого игрока уже максимум друзей"));
        return;
    }
    if (!m_friendService.addRequest(senderId, senderSession->serial, senderSession->accountId, targetId, now()))
    {
        // Либо своя заявка этому игроку уже висит, либо у него столько входящих, что
        // больше не влезает.
        sender.sendClientMessage(ERROR_COLOUR,
                                 u("Ваша заявка этому игроку уже отправлена либо у него слишком много заявок"));
        return;
    }

    // id отправителя подставлен прямо в команды: заявок может висеть несколько, и
    // строку остаётся только повторить. Цветокоды — наши; ник обезврежен, подделать
    // ими текст нельзя.
    target->sendClientMessage(
        INFO_COLOUR,
        u(fmt::format("{}[{}] хочет добавить вас в друзья. Принять заявку? {}/friendaccept {} {}/friendcancel {}",
                      safeName(sender), senderId, ACCEPT_COLOUR_CODE, senderId, CANCEL_COLOUR_CODE, senderId)));
    sender.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Заявка отправлена: {}[{}]. Ответ придёт в чат", safeName(*target),
                                           targetId)));
}

void FriendSystem::onAcceptCommand(IPlayer &player, int fromId)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы отвечать на заявки"));
        return;
    }
    const TimePoint timeNow = now();
    const FriendService::Request *request = m_friendService.requestFrom(playerId, fromId, timeNow);
    if (!request)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Заявки от игрока {} нет — она истекла или её не было", fromId)));
        const std::size_t pending = m_friendService.pendingCount(playerId, timeNow);
        if (pending > 0)
        {
            player.sendClientMessage(ERROR_COLOUR,
                                     u(fmt::format("Ждут ответа заявок: {}. Отвечать — /friendaccept <id>",
                                                   pending)));
        }
        return;
    }
    // Снимок ДО очистки: заявка лежит в списке и будет из него удалена.
    const std::uint32_t fromSerial = request->fromSerial;
    const FriendService::AccountId fromAccount = request->fromAccount;
    m_friendService.clearRequest(playerId, fromId);

    // Отправитель мог выйти, а слот — достаться другому игроку: сверяем сессию.
    IPlayer *sender = m_core.getPlayers().get(fromId);
    const PlayerSessionService::Session *senderSession = m_sessionService.get(fromId);
    if (!sender || !senderSession || senderSession->serial != fromSerial)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Приславший заявку уже не в игре"));
        return;
    }
    if (m_friendService.isFriend(playerId, fromAccount))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот игрок уже у вас в друзьях"));
        return;
    }

    FriendService::Friend forMe;
    forMe.accountId = fromAccount;
    forMe.name = std::string(sender->getName().data(), sender->getName().size());
    FriendService::Friend forSender;
    forSender.accountId = session->accountId;
    forSender.name = std::string(player.getName().data(), player.getName().size());

    if (!m_friendService.addFriend(playerId, std::move(forMe)) ||
        !m_friendService.addFriend(fromId, std::move(forSender)))
    {
        // Упёрлись в потолок у любой из сторон — дружбу не заводим совсем, иначе она
        // осталась бы односторонней.
        m_friendService.removeFriend(playerId, fromAccount);
        m_friendService.removeFriend(fromId, session->accountId);
        player.sendClientMessage(ERROR_COLOUR, u("Список друзей переполнен"));
        return;
    }

    // Дружба взаимная — пишем ОБЕ строки. IGNORE: повтор (успели подружиться другим
    // путём) не должен валить запрос.
    DatabaseManager::throwQuery(
        [own = session->accountId, fromAccount](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT IGNORE INTO player_friend (account_id, friend_id) VALUES (?, ?), (?, ?)")
                .bind(own, fromAccount, fromAccount, own)
                .execute();
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "FriendSystem: failed to save friendship: " + error);
        });

    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Теперь вы друзья с {}[{}]", safeName(*sender), fromId)));
    sender->sendClientMessage(INFO_COLOUR,
                              u(fmt::format("{}[{}] принял вашу заявку — теперь вы друзья", safeName(player),
                                            playerId)));
}

void FriendSystem::onCancelCommand(IPlayer &player, int fromId)
{
    const int playerId = player.getID();
    const TimePoint timeNow = now();
    const FriendService::Request *request = m_friendService.requestFrom(playerId, fromId, timeNow);
    if (!request)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Заявки от игрока {} нет — она истекла или её не было", fromId)));
        return;
    }
    const std::uint32_t fromSerial = request->fromSerial;
    m_friendService.clearRequest(playerId, fromId);

    player.sendClientMessage(INFO_COLOUR, u("Заявка отклонена"));

    // Отправителю сообщаем только если слот всё ещё за ним.
    const PlayerSessionService::Session *senderSession = m_sessionService.get(fromId);
    if (!senderSession || senderSession->serial != fromSerial)
    {
        return;
    }
    if (IPlayer *sender = m_core.getPlayers().get(fromId))
    {
        sender->sendClientMessage(ERROR_COLOUR,
                                  u(fmt::format("{}[{}] отклонил вашу заявку в друзья", safeName(player), playerId)));
    }
}

void FriendSystem::tickRequests()
{
    m_friendService.expireRequests(now(),
                                   [this](int toId, const FriendService::Request &request)
                                   {
                                       // Слот мог достаться другому игроку — сверяем сессию у обеих сторон.
                                       const PlayerSessionService::Session *senderSession =
                                           m_sessionService.get(request.fromId);
                                       if (senderSession && senderSession->serial == request.fromSerial)
                                       {
                                           if (IPlayer *sender = m_core.getPlayers().get(request.fromId))
                                           {
                                               sender->sendClientMessage(
                                                   ERROR_COLOUR, u("Ваша заявка в друзья осталась без ответа"));
                                           }
                                       }
                                       if (IPlayer *target = m_core.getPlayers().get(toId))
                                       {
                                           target->sendClientMessage(ERROR_COLOUR,
                                                                     u("Заявка в друзья истекла"));
                                       }
                                   });
}

// ------------------------------------------------------------------ лайфцикл сессии

void FriendSystem::loadFriends(IPlayer &player, const PlayerSessionService::Session &session)
{
    DatabaseManager::selectQuery<std::vector<FriendService::Friend>>(
        [accountId = session.accountId](mysqlx::Schema schema) -> std::vector<FriendService::Friend>
        {
            mysqlx::SqlResult result =
                schema.getSession()
                    .sql("SELECT p.id, p.name, DATE_FORMAT(p.last_seen, '%d.%m.%Y %H:%i') "
                         "FROM player_friend f JOIN player p ON p.id = f.friend_id "
                         "WHERE f.account_id = ? ORDER BY p.name")
                    .bind(accountId)
                    .execute();

            std::vector<FriendService::Friend> friends;
            while (mysqlx::Row row = result.fetchOne())
            {
                FriendService::Friend entry;
                entry.accountId = row.get(0).get<std::int64_t>();
                entry.name = row.get(1).get<std::string>();
                if (!row.get(2).isNull())
                {
                    entry.lastSeen = row.get(2).get<std::string>();
                }
                friends.push_back(std::move(entry));
            }
            return friends;
        },
        [this, playerId = player.getID(), serial = session.serial](std::vector<FriendService::Friend> friends)
        {
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
            {
                return;
            }
            m_friendService.loadFriends(playerId, std::move(friends));
            if (IPlayer *owner = m_core.getPlayers().get(playerId))
            {
                announceOnline(*owner);
            }
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "FriendSystem: failed to load friends: " + error);
        });
}

void FriendSystem::announceOnline(IPlayer &player)
{
    const int playerId = player.getID();
    // Дружба взаимная, поэтому «кому сообщить» — это ровно мой список друзей.
    const std::string name = safeName(player);
    for (const FriendService::Friend &entry : m_friendService.friendsOf(playerId))
    {
        const int onlineId = m_sessionService.playerByAccount(entry.accountId);
        if (onlineId < 0)
        {
            continue;
        }
        if (IPlayer *mate = m_core.getPlayers().get(onlineId))
        {
            mate->sendClientMessage(INFO_COLOUR, u(fmt::format("Ваш друг {}[{}] зашёл в игру", name, playerId)));
        }
    }
}

void FriendSystem::touchLastSeen(const PlayerSessionService::Session &session)
{
    // Ключ очереди на аккаунт: отметка не должна обгонять другие записи этой строки.
    DatabaseManager::throwQueryOrdered(
        "player:" + std::to_string(session.accountId),
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            schema.getSession().sql("UPDATE player SET last_seen = NOW() WHERE id = ?").bind(accountId).execute();
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "FriendSystem: failed to update last_seen: " + error);
        });
}

void FriendSystem::onSessionEnd(IPlayer &player)
{
    // Слот переиспользуется — чужой список друзей и чужие заявки не наследуем.
    m_friendService.resetPlayer(player.getID());
}
