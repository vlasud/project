#include "Systems/PhoneSystem/PhoneSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerChatService/PlayerChatService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Systems/Factions/PoliceLasVenturasSystem/PoliceLasVenturasSystem.h"
#include "Systems/Factions/PoliceLosSantosSystem/PoliceLosSantosSystem.h"
#include "Systems/Factions/PoliceSanFierroSystem/PoliceSanFierroSystem.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/MoneyFormat/MoneyFormat.h"
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};
// Вызов службы — отдельный канал: заметнее обычных информационных строк.
const Colour CALL_COLOUR{255, 200, 80};
// Сказанное в трубку слышно и вокруг говорящего — серым, как обычный чат: метка
// [Телефон] отличает эту реплику от сказанного вслух.
const Colour PHONE_COLOUR = Colour::FromRGBA(0xC0C0C0FF);
// А в самой трубке та же строка ЖЁЛТАЯ: собеседник должен отличать сказанное ему
// по телефону от того, что звучит рядом с ним.
const Colour PHONE_LINE_COLOUR = Colour::FromRGBA(0xFFD24AFF);
// Радиус слышимости телефонного разговора — как у обычного чата.
constexpr float PHONE_HEARING_RADIUS = 20.0f;

// --- видимая часть звонка ---
// Мобильник в правой руке. Оффсет и поворот подобраны под кость руки; при желании
// правятся нативной подгонкой (AttachmentService::beginEdit).
constexpr int PHONE_MODEL = 330;
const Vector3 PHONE_OFFSET{0.08f, 0.03f, -0.02f};
const Vector3 PHONE_ROTATION{-170.0f, -167.7f, 5.1f};

// Библиотека PED: достал и поднёс к уху / разговор / убрал.
constexpr const char *PHONE_ANIM_LIB = "PED";
constexpr const char *PHONE_ANIM_IN = "phone_in";
constexpr const char *PHONE_ANIM_TALK = "phone_talk";
constexpr const char *PHONE_ANIM_OUT = "phone_out";
// Сколько идут разовые анимации: по ним переключаемся на разговор и снимаем модель.
constexpr Milliseconds PHONE_IN_TIME{1200};
constexpr Milliseconds PHONE_OUT_TIME{1000};

// Гудок у звонящего и звонок у адресата — один и тот же «Calling tone» SA. Звук
// разовый, поэтому «непрерывный» гудок это повтор по таймеру, а выключение —
// остановка повтора (гасить уже играющий звук клиенту нечем).
constexpr std::uint32_t RING_SOUND = 3600;
constexpr Milliseconds RING_INTERVAL{2000};

// Звук уведомления о вызове (id от владельца): им же звонит входящий.
constexpr std::uint32_t CALL_SOUND = 17001;
// Звук кассы SA и попап покупки — как в общей витрине бизнеса.
constexpr std::uint32_t PURCHASE_SOUND = 1054;
constexpr Milliseconds PURCHASE_POPUP_TIME{2000};

// Сколько попыток подобрать случайный свободный номер при коллизии.
constexpr int PHONE_ISSUE_ATTEMPTS = 8;

// Пополнение счёта телефона: наличные меняются на счёт один к одному.
constexpr std::int64_t TOPUP_MIN = 10;
constexpr std::int64_t TOPUP_MAX = 100000;
// Попап тарификации висит меньше интервала списания — очередь попапов не растёт.
constexpr Milliseconds CHARGE_POPUP_TIME{2000};

// Имя UNIQUE-индекса номера: по нему в тексте ошибки БД узнаётся «номер занят».
// Числового кода у mysqlx::Error нет (getCode() есть только у Warning), а глушить
// подряд все ошибки нельзя: обрыв коннекта выглядел бы как занятый номер.
constexpr std::string_view PHONE_UNIQUE_INDEX = "uk_player_phone";

// Номер — ровно шесть цифр (PHONE_MIN..PHONE_MAX), длиннее не принимаем даже на
// разбор: ввод INPUT-диалога клиент не фильтрует вообще.
constexpr std::size_t PHONE_DIGITS = 6;

// Буфер строки разговора: тот же лимит client message, что и у чата.
constexpr std::size_t PHONE_LINE_SIZE = 128 + 1;
// Реплика в трубке длиннее чата: строка несёт ещё и метку с ником и id
// («[Телефон] текст : Ник[id]»), поэтому текст режется сильнее.
constexpr std::size_t PHONE_MAX_TEXT = 85;
// То же для SMS: метка короче на два байта — «[SMS] текст : Ник[id]».
constexpr std::size_t SMS_MAX_TEXT = 89;

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

// Метка реплики в трубке, сразу в cp1251: строка разговора собирается из
// клиентских байтов, и utf-8 литерал внутри неё дал бы кракозябры.
const std::string &phoneTag()
{
    static const std::string tag = u("Телефон");
    return tag;
}

// Ввод диалога может прийти пустым срезом с нулевым data() — конструировать из
// него string_view нельзя.
std::string_view viewOf(StringView text)
{
    return text.data() != nullptr ? std::string_view(text.data(), text.size()) : std::string_view{};
}

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    return text;
}

// Разбор введённого номера БЕЗ strtoll/atoi: клиентский ввод — произвольные байты,
// а длина проверяется до арифметики, поэтому переполниться нечему.
//  >0 — номер, 0 — поле пустое, -1 — мусор либо слишком длинно.
std::int64_t parsePhoneInput(std::string_view text)
{
    const std::string_view digits = trimmed(text);
    if (digits.empty())
    {
        return 0;
    }
    if (digits.size() > PHONE_DIGITS)
    {
        return -1;
    }
    std::int64_t value = 0;
    for (const char c : digits)
    {
        if (c < '0' || c > '9')
        {
            return -1;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

// Справка по услуге в витрине 24/7.
const char *phoneShopDescription()
{
    return "Телефон — это и есть ваш номер в сети. С ним можно звонить другим игрокам по номеру "
           "(/c 123456), а также вызывать скорую, полицию и такси.\n\n"
           "При покупке введите желаемый свободный шестизначный номер или оставьте поле пустым — "
           "тогда номер подберётся случайно.\n\n"
           "Телефон уже есть? Покупка МЕНЯЕТ номер: старый освобождается, деньги списываются заново, "
           "а идущий разговор прерывается.\n\n"
           "Номер живёт с аккаунтом и не пропадает при выходе из игры.";
}

// Справка по пополнению счёта в витрине 24/7.
const char *phoneTopUpDescription()
{
    return "Разговор по телефону платный: пока идёт звонок, со счёта телефона списывается "
           "плата за каждые несколько секунд связи. Платит тот, кто НАБРАЛ номер — "
           "входящие бесплатны.\n\n"
           "Деньги переводятся на счёт один к одному. Кончился счёт — разговор обрывается, "
           "и позвонить снова не выйдет, пока не пополните.\n\n"
           "Счёт привязан к аккаунту и переживает выход из игры. Смена номера его не сбрасывает.";
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
      m_audioService(serviceRegister.getService<AudioService>()),
      m_businessService(serviceRegister.getService<BusinessService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_noticeService(serviceRegister.getService<ScreenNoticeService>()),
      m_timers(serviceRegister.getService<TimerService>()),
      m_animationService(serviceRegister.getService<PlayerAnimationService>()),
      m_attachments(serviceRegister.getService<AttachmentService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_gridService(serviceRegister.getService<GridService>())
{
    m_phoneSlot.fill(-1);
    m_listeners.reserve(64);

    // Номер телефона аккаунта: только ЧИТАЕМ на старте сессии. Телефона нет —
    // молчим, игрок покупает его в 24/7.
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            loadPhone(player, session);
            // Первая анимация из ещё не загруженной либы клиентом не проигрывается —
            // иначе первый в сессии звонок прошёл бы без анимации вовсе.
            m_animationService.preloadLibrary(player, PHONE_ANIM_LIB);
        });
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            onSessionEnd(player);
        });
    // Погиб звонящий — вызов больше не актуален (иначе служба поедет к трупу), а
    // разговор обрывается: труп по телефону не говорит.
    m_healthService.subscribeDeath(
        [this](IPlayer &player)
        {
            const int playerId = player.getID();
            m_phoneService.cancelOrdersOf(playerId);
            dropCall(playerId, "Связь оборвалась");
        });

    // Речь в разговоре — обычным чатом: Core отдаёт копию уже прошедшей мут и
    // антиспам реплики и про телефон ничего не знает.
    serviceRegister.getService<PlayerChatService>().subscribeSpeech(
        [this](int playerId, StringView message)
        {
            return onSpeech(playerId, message);
        });

    // Телефон продаётся в 24/7 как УСЛУГА точки. Регистрирует её система-владелец
    // механики (как MedkitSystem — свой предмет), магазин не правится.
    BusinessService::GoodDef phone;
    phone.itemType = STOCK_PHONE; // ключ склада: предмета за этим числом нет
    phone.name = "Телефон";
    phone.price = PHONE_PRICE;
    phone.stockCap = PHONE_STOCK_CAP;
    phone.description = phoneShopDescription();
    phone.popupName = "Phone";
    phone.sell = [this](IPlayer &player, int businessId)
    {
        beginPurchase(player, businessId);
    };
    m_businessService.addGood(BusinessService::Type::Shop247, std::move(phone));

    // Вторая услуга той же точки: пополнение счёта. Отдельной строкой витрины —
    // купить телефон и положить на счёт это разные действия.
    BusinessService::ServiceDef topUp;
    topUp.name = "Счёт телефона";
    topUp.price = 0; // сумму называет игрок, фиксированной цены у пополнения нет
    topUp.description = phoneTopUpDescription();
    topUp.popupName = "Top up";
    topUp.status = [this](int playerId)
    {
        if (!m_phoneService.hasPhone(playerId))
        {
            return std::string("нет телефона");
        }
        return Money::text(m_phoneService.balanceOf(playerId));
    };
    topUp.sell = [this](IPlayer &player, int businessId)
    {
        beginTopUp(player, businessId);
    };
    m_businessService.addService(BusinessService::Type::Shop247, std::move(topUp));

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
            onCallCommand(player, std::string(viewOf(entered)));
        },
        {}, "позвонить: службы или номер игрока", PlayerCommandService::HelpCategory::Misc);

    commands.add(
        "pickup", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            onPickupCommand(player);
        },
        {}, "ответить на входящий звонок", PlayerCommandService::HelpCategory::Misc);

    commands.add(
        "hangup", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            onHangupCommand(player);
        },
        {}, "положить трубку: сбросить или завершить звонок", PlayerCommandService::HelpCategory::Misc);

    // Номер параметром-строкой, а не Int: разбираем своим побайтовым парсером, тем
    // же, что и на наборе. Текст — последний параметр, поэтому забирает весь хвост.
    commands.add(
        "sms", {{PlayerCommandService::Param::String, "номер телефона"},
                {PlayerCommandService::Param::String, "сообщение"}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            onSmsCommand(player, args.getString(0), args.getString(1));
        },
        {}, "отправить SMS на номер", PlayerCommandService::HelpCategory::Misc);

    commands.add(
        "acceptjob", {{PlayerCommandService::Param::Int, "номер вызова"}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            onAcceptJobCommand(player, args.getInt(0));
        },
        {}, "принять вызов по номеру (для работников служб)", PlayerCommandService::HelpCategory::Economy);
}

void PhoneSystem::initialize(IComponentList * /*components*/)
{
    // Один общий тик на все дозвоны: снимает неотвеченные вызовы по RING_TIMEOUT.
    // НИКОГДА не отменяется, в том числе из своего колбэка.
    m_callTimer = m_timers.setInterval(Milliseconds{1000},
                                       [this]()
                                       {
                                           tickCalls();
                                       });
}

// ------------------------------------------------------------------ команды

void PhoneSystem::onCallCommand(IPlayer &player, const std::string &argument)
{
    if (!requirePhone(player))
    {
        return;
    }
    if (argument.empty())
    {
        showCallMenu(player);
        return;
    }
    // Быстрый набор: /c <номер> — то же, что пункт «Ввести номер телефона».
    const std::int64_t parsed = parsePhoneInput(argument);
    if (parsed <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Номер телефона — шесть цифр. Без номера: /c"));
        return;
    }
    dialNumber(player, parsed);
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
    // Числовой ввод — через обёртку сервиса: мусор и overflow она отсекает сама,
    // повторно показывая тот же диалог. Диапазон проверяет dialNumber.
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Набор номера", "Введите номер телефона игрока, которому хотите позвонить",
                   "Позвонить", "Назад"),
        [this, playerId](DialogResponse response, std::int64_t number)
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
            dialNumber(*caller, number);
        });
}

// ------------------------------------------------------------------ вызовы служб

void PhoneSystem::placeServiceCall(IPlayer &player, PhoneService::Service service)
{
    const int callerId = player.getID();
    if (!requirePhone(player))
    {
        return;
    }
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

    // Телефона тут НЕ требуем: у служб рация, иначе таксист без телефона не смог бы
    // работать.
    //
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

// ------------------------------------------------------------------ эффекты звонка

void PhoneSystem::raisePhone(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId))
    {
        return;
    }
    // Отменяем незавершённый переход: он мог убирать телефон, который снова нужен.
    m_timers.cancel(m_animTimer[playerId]);
    if (m_phoneSlot[playerId] < 0)
    {
        m_phoneSlot[playerId] =
            m_attachments.attach(player, PHONE_MODEL, PlayerBone_RightHand, PHONE_OFFSET, PHONE_ROTATION);
    }
    m_phoneUp[playerId] = true;

    // В транспорте анимация телефона выглядит поломанной (она пешая) — там только
    // модель в руке.
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        return;
    }
    m_animationService.play(
        player, AnimationData(4.1f, false, false, false, false, 0, PHONE_ANIM_LIB, PHONE_ANIM_IN), true);
    m_animTimer[playerId] = m_timers.setPlayerTimeout(
        player, PHONE_IN_TIME,
        [this](IPlayer &owner)
        {
            const int id = owner.getID();
            if (!validPlayerId(id))
            {
                return;
            }
            m_animTimer[id] = TimerService::Handle{};
            // За время «достаёт телефон» звонок мог оборваться.
            if (!m_phoneUp[id] || m_stateService.getState(id) != PlayerState_OnFoot)
            {
                return;
            }
            m_animationService.play(
                owner, AnimationData(4.1f, true, false, false, false, 0, PHONE_ANIM_LIB, PHONE_ANIM_TALK), true);
        });
}

void PhoneSystem::lowerPhone(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId) || !m_phoneUp[playerId])
    {
        return; // телефона в руке нет либо его уже убирают
    }
    m_phoneUp[playerId] = false;
    m_timers.cancel(m_animTimer[playerId]);

    const int slot = m_phoneSlot[playerId];
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        m_attachments.detach(player, slot); // анимации не было — и убирать нечего
        m_phoneSlot[playerId] = -1;
        return;
    }
    m_animationService.play(
        player, AnimationData(4.1f, false, false, false, false, 0, PHONE_ANIM_LIB, PHONE_ANIM_OUT), true);
    // Модель снимается в КОНЦЕ обратной анимации: иначе рука убирала бы пустоту.
    m_animTimer[playerId] = m_timers.setPlayerTimeout(
        player, PHONE_OUT_TIME,
        [this, slot](IPlayer &owner)
        {
            const int id = owner.getID();
            if (!validPlayerId(id))
            {
                return;
            }
            m_animTimer[id] = TimerService::Handle{};
            if (m_phoneUp[id])
            {
                return; // за это время начался новый звонок — телефон снова нужен
            }
            m_attachments.detach(owner, slot);
            m_phoneSlot[id] = -1;
            m_animationService.stop(owner);
        });
}

void PhoneSystem::startRing(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId))
    {
        return;
    }
    m_timers.cancel(m_ringTimer[playerId]);
    m_audioService.playSound(player, RING_SOUND); // первый гудок — сразу
    m_ringTimer[playerId] = m_timers.setPlayerInterval(player, RING_INTERVAL,
                                                       [this](IPlayer &owner)
                                                       {
                                                           m_audioService.playSound(owner, RING_SOUND);
                                                       });
}

void PhoneSystem::stopRing(int playerId)
{
    if (!validPlayerId(playerId))
    {
        return;
    }
    m_timers.cancel(m_ringTimer[playerId]);
}

void PhoneSystem::endCallEffects(int playerId)
{
    stopRing(playerId);
    if (IPlayer *owner = m_core.getPlayers().get(playerId))
    {
        lowerPhone(*owner);
    }
}

void PhoneSystem::endPeerEffects(int peerId, std::uint32_t peerSerial)
{
    if (peerId < 0)
    {
        return;
    }
    const PlayerSessionService::Session *session = m_sessionService.get(peerId);
    if (!session || session->serial != peerSerial)
    {
        return; // слот уже за другим игроком — его звонок не наш
    }
    endCallEffects(peerId);
}

void PhoneSystem::clearPhoneProps(IPlayer &player)
{
    const int playerId = player.getID();
    if (!validPlayerId(playerId))
    {
        return;
    }
    m_timers.cancel(m_ringTimer[playerId]);
    m_timers.cancel(m_animTimer[playerId]);
    m_phoneUp[playerId] = false;
    if (m_phoneSlot[playerId] >= 0)
    {
        m_attachments.detach(player, m_phoneSlot[playerId]);
        m_phoneSlot[playerId] = -1;
    }
}

// ------------------------------------------------------------------ звонки игрокам

bool PhoneSystem::requirePhone(IPlayer &player)
{
    if (m_phoneService.hasPhone(player.getID()))
    {
        return true;
    }
    player.sendClientMessage(ERROR_COLOUR, u("Телефона у вас нет — звонить не с чего"));
    player.sendClientMessage(ERROR_COLOUR,
                             u(fmt::format("Купить телефон можно в магазине 24/7 за {}", Money::text(PHONE_PRICE))));
    return false;
}

void PhoneSystem::dialNumber(IPlayer &player, std::int64_t phone)
{
    const int callerId = player.getID();
    if (!requirePhone(player))
    {
        return;
    }
    if (phone < PhoneService::PHONE_MIN || phone > PhoneService::PHONE_MAX)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Номера телефонов — от {} до {}", PhoneService::PHONE_MIN,
                                               PhoneService::PHONE_MAX)));
        return;
    }
    const std::int64_t own = m_phoneService.phoneOf(callerId);
    if (phone == own)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Это ваш собственный номер"));
        return;
    }
    if (m_phoneService.callOf(callerId) != nullptr)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас уже идёт звонок — сначала /hangup"));
        return;
    }
    // Платит набирающий, поэтому пустой счёт отсекаем ДО дозвона: иначе абонент снял
    // бы трубку и связь тут же оборвалась на первом же списании.
    if (m_phoneService.balanceOf(callerId) < PhoneService::CALL_COST)
    {
        player.sendClientMessage(ERROR_COLOUR, u("На счету телефона нет денег на разговор"));
        player.sendClientMessage(ERROR_COLOUR, u("Пополнить счёт можно в магазине 24/7"));
        return;
    }
    const TimePoint timeNow = now();
    const int wait = m_phoneService.recallSecondsLeft(callerId, phone, timeNow);
    if (wait > 0)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Слишком часто: повторный вызов на этот номер через {} сек", wait)));
        return;
    }

    const PlayerSessionService::Session *callerSession = m_sessionService.get(callerId);
    if (!callerSession)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы звонить"));
        return;
    }

    const int calleeId = m_phoneService.playerByPhone(phone);
    const PlayerSessionService::Session *calleeSession =
        calleeId >= 0 ? m_sessionService.get(calleeId) : nullptr;
    IPlayer *callee = calleeId >= 0 ? m_core.getPlayers().get(calleeId) : nullptr;
    if (!calleeSession || !callee)
    {
        // ОДИН И ТОТ ЖЕ текст на «номера нет» и «абонент не в сети»: иначе перебором
        // номеров сканируется база аккаунтов.
        player.sendClientMessage(ERROR_COLOUR, u("Абонент вне зоны действия сети"));
        return;
    }
    if (m_phoneService.callOf(calleeId) != nullptr)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Занято — абонент сейчас разговаривает"));
        return;
    }

    if (!m_phoneService.startCall(callerId, callerSession->serial, own, calleeId, calleeSession->serial, phone,
                                  timeNow))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Не удалось соединить — попробуйте позже"));
        return;
    }

    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Идёт вызов абонента {}. Отменить — /hangup", phone)));
    // Адресату — НОМЕР звонящего, а не ник: до ответа собеседник неизвестен.
    callee->sendClientMessage(CALL_COLOUR, u(fmt::format("Входящий вызов с номера {}", own)));
    callee->sendClientMessage(CALL_COLOUR, u("Ответить — /pickup, сбросить — /hangup"));

    // Звонящий достаёт телефон сразу, адресат — только когда ответит.
    raisePhone(player);
    startRing(player);   // гудок ожидания
    startRing(*callee);  // звонок входящего
}

void PhoneSystem::onPickupCommand(IPlayer &player)
{
    const int playerId = player.getID();
    const PhoneService::CallLink *link = m_phoneService.callOf(playerId);
    if (!link)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сейчас вам никто не звонит"));
        return;
    }
    if (link->state == PhoneService::CallState::Active)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы уже разговариваете"));
        return;
    }
    if (link->state != PhoneService::CallState::Incoming)
    {
        // Свой исходящий «поднять» нельзя: иначе стороны разъедутся по состояниям.
        player.sendClientMessage(ERROR_COLOUR, u("Вы звоните сами — дождитесь ответа абонента"));
        return;
    }
    // Копируем ДО answer: после смены состояния ссылка описывает уже другое.
    const std::int64_t peerPhone = link->peerPhone;
    const std::uint32_t peerSerial = link->peerSerial;

    const int peerId = m_phoneService.answer(playerId, now());
    if (peerId < 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вызов уже сброшен"));
        return;
    }
    // Трубку сняли: гудок у обеих сторон смолкает, ответивший подносит телефон к уху.
    // Звонящему телефон уже в руке — он достал его при наборе.
    stopRing(playerId);
    const PlayerSessionService::Session *peerSession = m_sessionService.get(peerId);
    if (peerSession != nullptr && peerSession->serial == peerSerial)
    {
        stopRing(peerId);
    }
    raisePhone(player);

    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Соединение с номером {} установлено. Говорите обычным чатом, "
                                           "завершить — /hangup",
                                           peerPhone)));
    notifyPeer(peerId, peerSerial,
               fmt::format("Абонент {} ответил. Говорите обычным чатом, завершить — /hangup",
                           m_phoneService.phoneOf(playerId)));
}

void PhoneSystem::onHangupCommand(IPlayer &player)
{
    const int playerId = player.getID();
    const PhoneService::CallLink *link = m_phoneService.callOf(playerId);
    if (!link)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас нет ни звонка, ни разговора"));
        return;
    }
    // Снимок ДО разрыва: hangup обнуляет связку.
    const PhoneService::CallState state = link->state;
    const std::int64_t peerPhone = link->peerPhone;
    const std::uint32_t peerSerial = link->peerSerial;
    const std::int64_t own = m_phoneService.phoneOf(playerId);

    const int peerId = m_phoneService.hangup(playerId);

    // Трубку положили — телефон убирают обе стороны, гудок смолкает.
    endCallEffects(playerId);
    endPeerEffects(peerId, peerSerial);

    switch (state)
    {
    case PhoneService::CallState::Active:
        player.sendClientMessage(INFO_COLOUR, u("Разговор завершён"));
        notifyPeer(peerId, peerSerial, "Собеседник положил трубку");
        break;
    case PhoneService::CallState::Outgoing:
        player.sendClientMessage(INFO_COLOUR, u(fmt::format("Вызов абонента {} отменён", peerPhone)));
        notifyPeer(peerId, peerSerial, fmt::format("Вызов с номера {} отменён", own));
        break;
    case PhoneService::CallState::Incoming:
        player.sendClientMessage(INFO_COLOUR, u(fmt::format("Вызов с номера {} сброшен", peerPhone)));
        notifyPeer(peerId, peerSerial, "Абонент сбросил вызов");
        break;
    default:
        break;
    }
}

void PhoneSystem::onSmsCommand(IPlayer &sender, StringView numberArg, StringView text)
{
    const int senderId = sender.getID();
    if (!requirePhone(sender))
    {
        return;
    }
    const std::int64_t phone = parsePhoneInput(viewOf(numberArg));
    if (phone <= 0 || phone < PhoneService::PHONE_MIN || phone > PhoneService::PHONE_MAX)
    {
        sender.sendClientMessage(ERROR_COLOUR, u("Номер телефона — шесть цифр: /sms 123456 текст"));
        return;
    }
    if (phone == m_phoneService.phoneOf(senderId))
    {
        sender.sendClientMessage(ERROR_COLOUR, u("Это ваш собственный номер"));
        return;
    }
    // Пробелы клиент отдаёт как есть: сообщение из одних пробелов — пустое.
    if (trimmed(viewOf(text)).empty())
    {
        sender.sendClientMessage(ERROR_COLOUR, u("Пустое сообщение отправить нельзя"));
        return;
    }
    if (m_phoneService.balanceOf(senderId) < PhoneService::SMS_COST)
    {
        sender.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("На счету телефона нет {} на SMS",
                                               Money::text(PhoneService::SMS_COST))));
        sender.sendClientMessage(ERROR_COLOUR, u("Пополнить счёт можно в магазине 24/7"));
        return;
    }

    const int targetId = m_phoneService.playerByPhone(phone);
    IPlayer *target = targetId >= 0 ? m_core.getPlayers().get(targetId) : nullptr;
    const PlayerSessionService::Session *targetSession =
        targetId >= 0 ? m_sessionService.get(targetId) : nullptr;
    if (!target || !targetSession)
    {
        // Тот же текст, что и на наборе: иначе перебором номеров сканируется база.
        sender.sendClientMessage(ERROR_COLOUR, u("Абонент вне зоны действия сети"));
        return;
    }

    // Деньги снимаем ТОЛЬКО когда доставка уже гарантирована: адресат найден и жив.
    if (!m_phoneService.takeBalance(senderId, PhoneService::SMS_COST))
    {
        sender.sendClientMessage(ERROR_COLOUR, u("На счету телефона не хватило денег на SMS"));
        return;
    }

    if (text.size() > SMS_MAX_TEXT)
    {
        text.remove_suffix(text.size() - SMS_MAX_TEXT);
    }
    // Строка целиком в cp1251: текст и ник — клиентские байты. Текст АРГУМЕНТ формата.
    char buffer[PHONE_LINE_SIZE] = {0};
    const auto formatted = fmt::format_to_n(buffer, PHONE_LINE_SIZE - 1, "[SMS] {} : {}[{}]", text, sender.getName(),
                                            senderId);
    const std::size_t lineSize = formatted.size < PHONE_LINE_SIZE - 1 ? formatted.size : PHONE_LINE_SIZE - 1;
    Encoding::neutralizeLine(buffer, lineSize);
    target->sendClientMessage(PHONE_LINE_COLOUR, StringView(buffer, lineSize));

    const std::int64_t left = m_phoneService.balanceOf(senderId);
    if (const PlayerSessionService::Session *session = m_sessionService.get(senderId))
    {
        persistBalance(session->accountId, left);
    }
    sender.sendClientMessage(INFO_COLOUR, u(fmt::format("SMS отправлено на номер {}. Списано {}, на счету {}", phone,
                                                        Money::text(PhoneService::SMS_COST), Money::text(left))));
}

void PhoneSystem::notifyPeer(int peerId, std::uint32_t peerSerial, const std::string &message)
{
    if (peerId < 0)
    {
        return;
    }
    // Слот мог достаться другому игроку — сверяем сессию, чтобы не написать чужому.
    const PlayerSessionService::Session *session = m_sessionService.get(peerId);
    if (!session || session->serial != peerSerial)
    {
        return;
    }
    if (IPlayer *peer = m_core.getPlayers().get(peerId))
    {
        peer->sendClientMessage(INFO_COLOUR, u(message));
    }
}

void PhoneSystem::dropCall(int playerId, const std::string &reason)
{
    const PhoneService::CallLink *link = m_phoneService.callOf(playerId);
    if (!link)
    {
        return;
    }
    const std::uint32_t peerSerial = link->peerSerial;
    const int peerId = m_phoneService.hangup(playerId);
    endCallEffects(playerId);
    endPeerEffects(peerId, peerSerial);
    notifyPeer(peerId, peerSerial, reason);
}

void PhoneSystem::tickCalls()
{
    m_phoneService.expireRinging(
        now(),
        [this](const PhoneService::ExpiredRing &ring)
        {
            // Слот мог достаться другому игроку — обе стороны через notifyPeer,
            // он сверяет серию сессии.
            endPeerEffects(ring.callerId, ring.callerSerial);
            endPeerEffects(ring.calleeId, ring.calleeSerial);
            notifyPeer(ring.callerId, ring.callerSerial, "Абонент не отвечает — вызов завершён");
            // Номер звонящего берём из кэша ТОЛЬКО пока слот за его сессией: иначе
            // это уже номер другого игрока.
            const PlayerSessionService::Session *callerSession = m_sessionService.get(ring.callerId);
            const std::int64_t from = callerSession != nullptr && callerSession->serial == ring.callerSerial
                                          ? m_phoneService.phoneOf(ring.callerId)
                                          : PhoneService::NO_PHONE;
            notifyPeer(ring.calleeId, ring.calleeSerial,
                       from == PhoneService::NO_PHONE ? std::string("Пропущенный вызов")
                                                      : fmt::format("Пропущенный вызов с номера {}", from));
        });

    // Тарификация разговора. Отметку времени двигает сам сервис, поэтому за один
    // интервал списание случается ровно однажды.
    m_phoneService.collectCharges(now(),
                                  [this](const PhoneService::CallCharge &charge)
                                  {
                                      const PlayerSessionService::Session *session =
                                          m_sessionService.get(charge.payerId);
                                      IPlayer *payer = m_core.getPlayers().get(charge.payerId);
                                      if (!payer || !session || session->serial != charge.payerSerial)
                                      {
                                          return; // слот уже за другим игроком — платить некому
                                      }
                                      if (chargeCall(*payer))
                                      {
                                          return;
                                      }
                                      // Счёт кончился прямо в разговоре — связь обрывается у обоих.
                                      const int peerId = m_phoneService.hangup(charge.payerId);
                                      endCallEffects(charge.payerId);
                                      endPeerEffects(peerId, charge.peerSerial);
                                      payer->sendClientMessage(
                                          ERROR_COLOUR, u("На счету телефона кончились деньги — разговор прерван"));
                                      payer->sendClientMessage(
                                          ERROR_COLOUR, u("Пополнить счёт можно в магазине 24/7"));
                                      m_noticeService.show(*payer, "Out of credit", CHARGE_POPUP_TIME, ERROR_COLOUR);
                                      notifyPeer(peerId, charge.peerSerial, "Связь оборвалась");
                                  });
}

bool PhoneSystem::chargeCall(IPlayer &payer)
{
    const int payerId = payer.getID();
    if (!m_phoneService.takeBalance(payerId, PhoneService::CALL_COST))
    {
        return false;
    }
    const std::int64_t left = m_phoneService.balanceOf(payerId);

    // Попапы проекта всегда английские.
    m_noticeService.show(payer, fmt::format("-{} - {} left", Money::text(PhoneService::CALL_COST), Money::text(left)),
                         CHARGE_POPUP_TIME, INFO_COLOUR);

    if (const PlayerSessionService::Session *session = m_sessionService.get(payerId))
    {
        persistBalance(session->accountId, left);
    }
    return true;
}

void PhoneSystem::persistBalance(PlayerSessionService::AccountId accountId, std::int64_t balance)
{
    // Ключ очереди на аккаунт: списания и пополнения одного счёта обязаны лечь в БД
    // в том же порядке, в каком поменяли кэш, иначе последним останется устаревшее.
    DatabaseManager::throwQueryOrdered(
        "player:" + std::to_string(accountId),
        [accountId, balance](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("UPDATE player SET phone_balance = ? WHERE id = ?")
                .bind(balance, accountId)
                .execute();
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "PhoneSystem: failed to persist phone balance: " + error);
        });
}

bool PhoneSystem::onSpeech(int playerId, StringView message)
{
    const PhoneService::CallLink *link = m_phoneService.callOf(playerId);
    if (!link || link->state != PhoneService::CallState::Active)
    {
        return false; // не по телефону — обычная реплика, чат разошлёт её сам
    }
    IPlayer *speaker = m_core.getPlayers().get(playerId);
    if (!speaker)
    {
        return false;
    }
    // Собеседник мог выйти, а слот — достаться другому: в трубку тогда не пишем, но
    // реплику всё равно забираем себе (говорящий держит телефон у уха).
    const int peerId = link->peerId;
    const PlayerSessionService::Session *session = m_sessionService.get(peerId);
    IPlayer *peer = session != nullptr && session->serial == link->peerSerial ? m_core.getPlayers().get(peerId)
                                                                             : nullptr;

    // Чат уже подрезал реплику под свою строку, но наша длиннее на ник и id —
    // режем ещё, чтобы хвост «: Ник[id]» не срезало форматированием.
    if (message.size() > PHONE_MAX_TEXT)
    {
        message.remove_suffix(message.size() - PHONE_MAX_TEXT);
    }

    // Строка целиком в cp1251: текст и ник пришли от клиента как есть, метка
    // сконвертирована один раз. Текст — АРГУМЕНТ формата, не строка формата.
    char buffer[PHONE_LINE_SIZE] = {0};
    const auto formatted = fmt::format_to_n(buffer, PHONE_LINE_SIZE - 1, "[{}] {} : {}[{}]", phoneTag(), message,
                                            speaker->getName(), playerId);
    const std::size_t lineSize = formatted.size < PHONE_LINE_SIZE - 1 ? formatted.size : PHONE_LINE_SIZE - 1;
    // Цветокоды и управляющие байты клиентского текста — как в локальном чате.
    Encoding::neutralizeLine(buffer, lineSize);
    const StringView line(buffer, lineSize);

    // Из трубки — ЖЁЛТЫМ: собеседник должен отличать сказанное ему по телефону от
    // того, что слышно вокруг него самого.
    if (peer != nullptr)
    {
        peer->sendClientMessage(PHONE_LINE_COLOUR, line);
    }

    // Вслух — та же строка серым, себе и стоящим рядом: половину разговора слышно.
    // Радиус и фильтр мира — как у обычного чата.
    const Vector3 position = m_locationService.getPosition(playerId);
    const int virtualWorld = m_locationService.getVirtualWorld(playerId);
    m_gridService.queryRadius(position, PHONE_HEARING_RADIUS, gridMask(GridEntityType::Player), m_listeners);
    for (const GridService::Result &listener : m_listeners)
    {
        // Собеседника пропускаем: он уже получил эту реплику из трубки, вторая
        // такая же строка выглядела бы дублем.
        if (listener.id == peerId || m_locationService.getVirtualWorld(listener.id) != virtualWorld)
        {
            continue;
        }
        if (IPlayer *target = m_core.getPlayers().get(listener.id))
        {
            target->sendClientMessage(PHONE_COLOUR, line);
        }
    }
    return true; // реплику забрали: обычный чат её не покажет
}

// ------------------------------------------------------------------ пополнение счёта

void PhoneSystem::beginTopUp(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    if (!m_sessionService.isActive(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы пополнить счёт"));
        return;
    }
    // Класть на счёт без телефона бессмысленно: счёт живёт вместе с номером.
    if (!m_phoneService.hasPhone(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сначала купите телефон — счёт пополнять пока нечему"));
        return;
    }
    showTopUpDialog(player, businessId, "");
}

void PhoneSystem::showTopUpDialog(IPlayer &player, int businessId, const std::string &hint)
{
    const int playerId = player.getID();

    std::string body = fmt::format("На счету сейчас {}\n", Money::text(m_phoneService.balanceOf(playerId)));
    body += fmt::format("Минута разговора стоит {} за каждые {} секунд связи\n", Money::text(PhoneService::CALL_COST),
                        static_cast<int>(PhoneService::CALL_CHARGE_INTERVAL.count()));
    body += fmt::format("Введите сумму пополнения: от {} до {}\n", Money::text(TOPUP_MIN), Money::text(TOPUP_MAX));
    if (!hint.empty())
    {
        body += "\n" + hint;
    }

    // Числовой ввод — через обёртку сервиса: мусор и overflow она отсекает сама.
    // Остаётся проверить только диапазон — про него обёртка не знает.
    m_dialogService.showNumberInput(
        player, makeDialog(DialogStyle_INPUT, "Счёт телефона", body, "Пополнить", "Отмена"),
        [this, playerId, businessId](DialogResponse response, std::int64_t amount)
        {
            IPlayer *buyer = m_core.getPlayers().get(playerId);
            if (!buyer || response != DialogResponse_Left)
            {
                return;
            }
            if (amount < TOPUP_MIN || amount > TOPUP_MAX)
            {
                showTopUpDialog(*buyer, businessId,
                                fmt::format("Пополнить можно от {} до {}", Money::text(TOPUP_MIN),
                                            Money::text(TOPUP_MAX)));
                return;
            }
            topUp(*buyer, businessId, amount);
        });
}

void PhoneSystem::topUp(IPlayer &player, int businessId, std::int64_t amount)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы пополнить счёт"));
        return;
    }
    // Телефон могли сменить, пока висел диалог, — но номер при этом остаётся; а вот
    // если телефона нет вовсе, класть некуда.
    if (!m_phoneService.hasPhone(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сначала купите телефон — счёт пополнять пока нечему"));
        return;
    }
    if (!m_moneyService.take(player, static_cast<unsigned long long>(amount)))
    {
        showTopUpDialog(player, businessId, fmt::format("Не хватает наличных: нужно {}", Money::text(amount)));
        return;
    }

    // Счёт — серверный кэш, в БД уходит следом по очереди аккаунта.
    const std::int64_t balance = m_phoneService.balanceOf(playerId) + amount;
    m_phoneService.setBalance(playerId, balance);
    persistBalance(session->accountId, balance);

    // Выручка — в копилку точки, как и за телефон.
    m_businessService.addIncome(businessId, amount);

    m_noticeService.show(player, "Top up", PURCHASE_POPUP_TIME, INFO_COLOUR);
    m_audioService.playSound(player, PURCHASE_SOUND);
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Счёт телефона пополнен на {}. Сейчас на счету {}",
                                                        Money::text(amount), Money::text(balance))));
}

// ------------------------------------------------------------------ покупка телефона

void PhoneSystem::beginPurchase(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    if (!m_sessionService.isActive(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы купить телефон"));
        return;
    }
    // Телефоны на точке кончились — отказ СРАЗУ. Витрина это уже проверила, но между
    // её проверкой и подтверждением диалога проходит время.
    if (m_businessService.stockOf(businessId, STOCK_PHONE) <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Телефоны закончились — товар не завезли"));
        return;
    }
    // Денег не хватает — отказ СРАЗУ: ни диалога, ни запроса в БД.
    if (!m_moneyService.canAfford(playerId, static_cast<unsigned long long>(PHONE_PRICE)))
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Не хватает денег: телефон стоит {}", Money::text(PHONE_PRICE))));
        return;
    }
    showNumberDialog(player, businessId, "");
}

void PhoneSystem::showNumberDialog(IPlayer &player, int businessId, const std::string &hint)
{
    const int playerId = player.getID();
    const std::int64_t own = m_phoneService.phoneOf(playerId);

    std::string body = fmt::format("Телефон — {}\nНомер шестизначный: от {} до {}\n", Money::text(PHONE_PRICE),
                                   PhoneService::PHONE_MIN, PhoneService::PHONE_MAX);
    body += "Введите желаемый свободный номер или оставьте поле пустым — подберём случайный\n";
    if (own != PhoneService::NO_PHONE)
    {
        body += fmt::format("Ваш текущий номер {} — покупка ЗАМЕНИТ его\n", own);
    }
    if (!hint.empty())
    {
        body += "\n" + hint;
    }

    // ЕДИНСТВЕННЫЙ числовой ввод телефона МИМО showNumberInput, и намеренно: здесь
    // ПУСТОЕ поле — осмысленный ответ «подбери номер сам». Обёртка же считает
    // нераспарсенный ввод ошибкой и повторяет диалог, то есть случайный номер стал бы
    // недостижим. Поэтому разбор свой, побайтовый (parsePhoneInput).
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Покупка телефона", body, "Купить", "Отмена"),
        [this, playerId, businessId](DialogResponse response, int, StringView text)
        {
            IPlayer *buyer = m_core.getPlayers().get(playerId);
            if (!buyer || response != DialogResponse_Left)
            {
                return;
            }
            // Ввод — сырые байты клиента; цифры одинаковы в cp1251 и utf-8, поэтому
            // проверяем побайтово и без конвертации.
            const std::int64_t desired = parsePhoneInput(viewOf(text));
            if (desired < 0)
            {
                showNumberDialog(*buyer, businessId, "Номер — не больше шести цифр, только цифры");
                return;
            }
            if (desired > 0 && (desired < PhoneService::PHONE_MIN || desired > PhoneService::PHONE_MAX))
            {
                showNumberDialog(*buyer, businessId,
                                 fmt::format("Номер бывает от {} до {}", PhoneService::PHONE_MIN,
                                             PhoneService::PHONE_MAX));
                return;
            }
            if (desired > 0 && desired == m_phoneService.phoneOf(playerId))
            {
                showNumberDialog(*buyer, businessId, "Это ваш текущий номер — платить за него незачем");
                return;
            }
            claimNumber(*buyer, businessId, desired);
        });
}

void PhoneSystem::claimNumber(IPlayer &player, int businessId, std::int64_t desired)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы купить телефон"));
        return;
    }
    // Одна заявка на игрока: два запроса в полёте списали бы деньги дважды за один
    // номер, а компенсация второго обнулила бы уже выданный.
    if (!m_phoneService.beginNumberRequest(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Предыдущая покупка ещё обрабатывается — подождите"));
        return;
    }
    if (!m_moneyService.canAfford(playerId, static_cast<unsigned long long>(PHONE_PRICE)))
    {
        m_phoneService.endNumberRequest(playerId);
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Не хватает денег: телефон стоит {}", Money::text(PHONE_PRICE))));
        return;
    }

    const PlayerSessionService::AccountId accountId = session->accountId;
    const std::uint32_t serial = session->serial;
    // Номер, который придётся вернуть, если покупка сорвётся после занятия.
    const std::int64_t previous = m_phoneService.phoneOf(playerId);

    DatabaseManager::selectQuery<std::int64_t>(
        [accountId, desired](mysqlx::Schema schema) -> std::int64_t
        {
            // Занять номер = ОДИН UPDATE. Занятость ловит UNIQUE (uk_player_phone):
            // конфликт прилетает исключением. Старый номер аккаунта освобождается
            // этим же UPDATE — перезаписью значения, отдельного шага не нужно.
            const auto claim = [&schema, accountId](std::int64_t candidate) -> bool
            {
                try
                {
                    schema.getSession()
                        .sql("UPDATE player SET phone = ? WHERE id = ?")
                        .bind(candidate, accountId)
                        .execute();
                }
                catch (const mysqlx::Error &error)
                {
                    // Занятый номер — нарушение uk_player_phone; всё остальное
                    // (обрыв, дедлок, недокатанная схема) пробрасываем, иначе сбой БД
                    // молча превратится в «номер занят» и не попадёт в лог.
                    if (std::string_view(error.what()).find(PHONE_UNIQUE_INDEX) == std::string_view::npos)
                    {
                        throw;
                    }
                    return false;
                }
                return true;
            };

            if (desired != 0)
            {
                return claim(desired) ? desired : PhoneService::NO_PHONE;
            }
            std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<std::int64_t> dist(PhoneService::PHONE_MIN, PhoneService::PHONE_MAX);
            for (int attempt = 0; attempt < PHONE_ISSUE_ATTEMPTS; ++attempt)
            {
                const std::int64_t candidate = dist(rng);
                if (claim(candidate))
                {
                    return candidate;
                }
            }
            return PhoneService::NO_PHONE;
        },
        [this, playerId, serial, accountId, businessId, desired, previous](std::int64_t claimed)
        {
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            const bool sameSession = current != nullptr && current->serial == serial;
            if (sameSession)
            {
                m_phoneService.endNumberRequest(playerId);
            }
            IPlayer *buyer = m_core.getPlayers().get(playerId);

            if (claimed == PhoneService::NO_PHONE)
            {
                // Номер не занят — в БД ничего не поменялось, деньги не тронуты.
                if (buyer && sameSession)
                {
                    buyer->sendClientMessage(ERROR_COLOUR,
                                             u(desired != 0
                                                   ? fmt::format("Номер {} занят — выберите другой", desired)
                                                   : std::string("Свободный номер не нашёлся — попробуйте ещё раз")));
                }
                return;
            }
            // Номер УЖЕ занят за аккаунтом. Любая неудача дальше обязана его вернуть,
            // иначе телефон достаётся бесплатно (вышел в момент запроса — перезашёл).
            if (!buyer || !sameSession)
            {
                rollbackNumber(accountId, claimed, previous, playerId, serial);
                return;
            }
            if (!m_moneyService.take(*buyer, static_cast<unsigned long long>(PHONE_PRICE)))
            {
                // Деньги успели потратить, пока висел диалог.
                rollbackNumber(accountId, claimed, previous, playerId, serial);
                buyer->sendClientMessage(ERROR_COLOUR,
                                         u(fmt::format("Не хватает денег: телефон стоит {}",
                                                       Money::text(PHONE_PRICE))));
                return;
            }
            // Склад списываем ПОСЛЕ денег, но ДО выдачи номера: последний телефон
            // могли забрать, пока мы ходили в БД за номером.
            if (!m_businessService.consumeStock(businessId, STOCK_PHONE, 1))
            {
                m_moneyService.giveMoney(*buyer, static_cast<unsigned long long>(PHONE_PRICE));
                rollbackNumber(accountId, claimed, previous, playerId, serial);
                buyer->sendClientMessage(ERROR_COLOUR, u("Последний телефон только что забрали"));
                return;
            }
            completePurchase(*buyer, businessId, claimed);
        },
        [this, playerId, serial](const std::string &error)
        {
            // Запрос не выполнился вовсе (ошибка БД до/вне UPDATE): номер не занят,
            // откатывать нечего.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (current && current->serial == serial)
            {
                m_phoneService.endNumberRequest(playerId);
                if (IPlayer *buyer = m_core.getPlayers().get(playerId))
                {
                    buyer->sendClientMessage(ERROR_COLOUR, u("Не удалось оформить номер — попробуйте позже"));
                }
            }
            LogManager::log(Error, "PhoneSystem: failed to claim a phone number: " + error);
        });
}

void PhoneSystem::rollbackNumber(PlayerSessionService::AccountId accountId, std::int64_t claimed,
                                 std::int64_t previous, int playerId, std::uint32_t serial)
{
    if (claimed == PhoneService::NO_PHONE)
    {
        return;
    }
    // Возвращает true, если аккаунту вернулся ПРЕЖНИЙ номер: только тогда кэш
    // игрока (его никто не менял) по-прежнему совпадает с БД.
    DatabaseManager::selectQuery<bool>(
        [accountId, claimed, previous](mysqlx::Schema schema) -> bool
        {
            // Условие phone = claimed обязательно: за это время номер аккаунта мог
            // смениться легально, и затирать ту запись нельзя.
            if (previous != PhoneService::NO_PHONE)
            {
                try
                {
                    schema.getSession()
                        .sql("UPDATE player SET phone = ? WHERE id = ? AND phone = ?")
                        .bind(previous, accountId, claimed)
                        .execute();
                    return true;
                }
                catch (const mysqlx::Error &)
                {
                    // Старый номер уже кем-то занят (мы его освободили) — снимаем
                    // номер вовсе, телефон придётся покупать заново.
                }
            }
            schema.getSession()
                .sql("UPDATE player SET phone = NULL WHERE id = ? AND phone = ?")
                .bind(accountId, claimed)
                .execute();
            return false;
        },
        [this, playerId, serial, previous](bool restored)
        {
            if (restored || previous == PhoneService::NO_PHONE)
            {
                return; // в БД лежит то же, что в кэше
            }
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
            {
                return; // игрок ушёл — кэш слота уже очищен концом сессии
            }
            // Номер потерян: держать его в кэше нельзя, он мог достаться другому.
            dropCall(playerId, "Связь прервана: номер абонента больше не обслуживается");
            m_phoneService.setPhone(playerId, PhoneService::NO_PHONE);
            if (IPlayer *owner = m_core.getPlayers().get(playerId))
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Ваш прежний номер успели занять — телефон снят"));
                owner->sendClientMessage(ERROR_COLOUR, u("Купите телефон заново в магазине 24/7"));
            }
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "PhoneSystem: failed to roll back a phone number: " + error);
        });
}

void PhoneSystem::completePurchase(IPlayer &player, int businessId, std::int64_t phone)
{
    const int playerId = player.getID();
    // Смена номера рвёт разговор: собеседник звонил на старый номер.
    dropCall(playerId, "Связь прервана: абонент сменил номер");
    m_phoneService.setPhone(playerId, phone);

    // Выручка — в копилку точки. Бизнес мог быть снесён, пока висел диалог, — тогда
    // доход просто некуда класть, это не ошибка.
    m_businessService.addIncome(businessId, PHONE_PRICE);

    m_noticeService.show(player, "Phone", PURCHASE_POPUP_TIME, INFO_COLOUR);
    m_audioService.playSound(player, PURCHASE_SOUND);
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Телефон куплен за {}. Ваш номер: {}",
                                                        Money::text(PHONE_PRICE), phone)));
    player.sendClientMessage(INFO_COLOUR,
                             u("Позвонить — /c <номер>, ответить — /pickup, положить трубку — /hangup"));
}

// ------------------------------------------------------------------ лайфцикл сессии

void PhoneSystem::loadPhone(IPlayer &player, const PlayerSessionService::Session &session)
{
    // ТОЛЬКО чтение: номер покупается в 24/7, автовыдачи на входе нет. Счёт читаем
    // тем же запросом — отдельный поход в БД за соседней колонкой не нужен.
    DatabaseManager::selectQuery<std::pair<std::int64_t, std::int64_t>>(
        [accountId = session.accountId](mysqlx::Schema schema) -> std::pair<std::int64_t, std::int64_t>
        {
            mysqlx::RowResult result = schema.getTable("player")
                                           .select("phone", "phone_balance")
                                           .where("id = :id")
                                           .limit(1)
                                           .bind("id", accountId)
                                           .execute();
            std::int64_t phone = PhoneService::NO_PHONE;
            std::int64_t balance = 0;
            if (mysqlx::Row row = result.fetchOne())
            {
                if (!row.get(0).isNull())
                {
                    phone = row.get(0).get<std::int64_t>();
                }
                if (!row.get(1).isNull())
                {
                    balance = row.get(1).get<std::int64_t>();
                }
            }
            return {phone, balance};
        },
        [this, playerId = player.getID(), serial = session.serial](std::pair<std::int64_t, std::int64_t> loaded)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
            {
                return;
            }
            const auto [phone, balance] = loaded;
            if (phone == PhoneService::NO_PHONE)
            {
                return; // телефона нет — молчим, счёт без номера ни на что не влияет
            }
            m_phoneService.setPhone(playerId, phone);
            m_phoneService.setBalance(playerId, balance);
            if (IPlayer *owner = m_core.getPlayers().get(playerId))
            {
                owner->sendClientMessage(INFO_COLOUR,
                                         u(fmt::format("Ваш номер телефона: {}. Позвонить — /c", phone)));
                owner->sendClientMessage(INFO_COLOUR,
                                         u(fmt::format("На счету телефона {}. Пополнить — в магазине 24/7",
                                                       Money::text(m_phoneService.balanceOf(playerId)))));
            }
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "PhoneSystem: failed to load phone number: " + error);
        });
}

void PhoneSystem::onSessionEnd(IPlayer &player)
{
    const int playerId = player.getID();
    // Серию собеседника снимаем ДО сброса: reset обнуляет связку.
    const PhoneService::CallLink *link = m_phoneService.callOf(playerId);
    const std::uint32_t peerSerial = link ? link->peerSerial : 0;
    // Слот переиспользуется — чужой номер, вызовы, звонок и телефон в руке не
    // наследуем.
    const int peerId = m_phoneService.resetPlayer(playerId);
    clearPhoneProps(player);
    endPeerEffects(peerId, peerSerial);
    notifyPeer(peerId, peerSerial, "Связь прервана: абонент вне зоны действия сети");
}
