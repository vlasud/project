#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

class PhoneSystem;

// Телефон — бизнес-сервис (НЕ Core). Держит три вещи:
//
//  * НОМЕРА ТЕЛЕФОНОВ игроков онлайн (кэш; персистятся в player.phone). Телефон
//    ПОКУПАЕТСЯ в 24/7 (см. Docs/Phone.md), автовыдачи нет: пустой кэш = телефона
//    у игрока нет.
//  * ВЫЗОВЫ СЛУЖБ: заказ такси/скорой/полиции с уникальным номером, который
//    работник принимает командой.
//  * ЗВОНКИ игрок<->игрок: пер-игроковая связка CallLink, всегда парная.
//
// СЛУЖБЫ РЕГИСТРИРУЮТСЯ САМИ (как в JobDismissService/JobWalletService): работа
// приносит перечислитель «кто у меня сейчас на смене», а сервис про работы ничего не
// знает. Новая служба — одна строка регистрации, телефон не правится.
//
// Заказ живёт до принятия либо до истечения ORDER_TTL. Больше ОДНОГО активного заказа
// на игрока нет — иначе один звонящий завалил бы уведомлениями всех работников сразу.
class PhoneService final : public IService
{
    friend PhoneSystem;

  public:
    enum class Service
    {
        Ambulance,
        Police,
        Taxi,
        Count
    };

    // Никто не принял — заказ протухает, и звонящий может позвать снова.
    static constexpr std::chrono::minutes ORDER_TTL{5};

    // Номер телефона: шестизначный, чтобы его можно было продиктовать и набрать.
    static constexpr std::int64_t PHONE_MIN = 100000;
    static constexpr std::int64_t PHONE_MAX = 999999;
    static constexpr std::int64_t NO_PHONE = 0;

    // Сколько звонит неотвеченный вызов, прежде чем снимется у обеих сторон.
    static constexpr std::chrono::seconds RING_TIMEOUT{10};
    // Пауза перед повторным вызовом ТОМУ ЖЕ номеру: иначе сбросом и повтором
    // абонента заваливают звонком без остановки.
    static constexpr std::chrono::seconds RECALL_COOLDOWN{10};

    // Тариф разговора: сколько и как часто снимается со счёта телефона. Платит
    // ЗВОНЯЩИЙ — иначе входящим звонком можно было бы обнулить чужой счёт.
    static constexpr std::int64_t CALL_COST = 3;
    static constexpr std::chrono::seconds CALL_CHARGE_INTERVAL{5};

    enum class CallState
    {
        None,     // звонка нет
        Outgoing, // я звоню, жду ответа
        Incoming, // звонят мне
        Active    // разговор идёт
    };

    // Половина звонка. Симметрична: у A peer = B <=> у B peer = A, поэтому рвать
    // звонок можно только ПАРОЙ (см. hangup).
    struct CallLink
    {
        CallState state = CallState::None;
        int peerId = -1;
        std::uint32_t peerSerial = 0; // сессия собеседника: слот playerId переиспользуется
        std::int64_t peerPhone = 0;   // НОМЕР собеседника: в разговоре ник не раскрываем
        TimePoint startedAt;          // старт дозвона (для RING_TIMEOUT), затем старт разговора
        bool initiator = false;       // эта сторона набирала номер — она и платит за разговор
        TimePoint chargedAt;          // когда с этой стороны сняли за разговор в последний раз
    };

    struct Order
    {
        int number = 0; // уникальный номер заказа (то, что вводят в команде приёма)
        Service service = Service::Ambulance;
        int callerId = -1;
        std::uint32_t callerSerial = 0; // сессия звонящего: слот playerId переиспользуется
        Vector3 position{};             // где звонящий был в момент вызова
        TimePoint createdAt;
    };

    // Кому уходит уведомление о вызове: работа сама решает, кто у неё «на смене».
    using WorkerVisitor = std::function<void(int playerId)>;
    using DispatchEnumerator = std::function<void(const WorkerVisitor &)>;

    // Название службы в родительном падеже — подставляется в «Вызов {} номер N»
    // («скорой помощи», «полиции», «такси»).
    void registerDispatch(Service service, std::string callName, DispatchEnumerator enumerate);
    const std::string &callNameOf(Service service) const;
    // Обойти работников службы. Служба не зарегистрирована — обход пустой.
    void forEachWorker(Service service, const WorkerVisitor &visitor) const;
    bool hasDispatch(Service service) const;

    // --- номера телефонов ---
    std::int64_t phoneOf(int playerId) const;    // NO_PHONE — телефона у игрока нет
    bool hasPhone(int playerId) const;
    int playerByPhone(std::int64_t phone) const; // -1 — такого номера нет в сети

    // --- счёт телефона ---
    // Кэш онлайн-игроков; правда — player.phone_balance, пишет PhoneSystem.
    std::int64_t balanceOf(int playerId) const;

    // --- звонки игрок<->игрок ---
    // Поставить Outgoing звонящему и Incoming адресату. false — любая сторона занята,
    // id невалиден или это звонок самому себе. Заодно взводит паузу повторного
    // вызова на номер адресата.
    bool startCall(int callerId, std::uint32_t callerSerial, std::int64_t callerPhone, int calleeId,
                   std::uint32_t calleeSerial, std::int64_t calleePhone, TimePoint now);
    // Incoming -> Active у обеих сторон. Возвращает peerId (-1 — отвечать нечего;
    // рассогласованная пара при этом снимается целиком).
    int answer(int playerId, TimePoint now);
    // Снять звонок у обеих сторон. Возвращает peerId, которому нужно сообщить (-1 —
    // звонка не было либо пара уже рассогласована).
    int hangup(int playerId);
    const CallLink *callOf(int playerId) const; // nullptr — звонка нет
    // Сколько секунд ещё нельзя звонить на этот номер (0 — можно).
    int recallSecondsLeft(int playerId, std::int64_t phone, TimePoint now) const;

    // Снятый по таймауту дозвон: id обеих сторон и СЕРИИ их сессий (связка к
    // моменту уведомления уже стёрта, а слот мог достаться другому игроку).
    struct ExpiredRing
    {
        int callerId = -1;
        std::uint32_t callerSerial = 0;
        int calleeId = -1;
        std::uint32_t calleeSerial = 0;
    };

    // Снять протухшие дозвоны (RING_TIMEOUT) и отдать снятые пары системе — она
    // уведомляет. Визитор зовётся ПОСЛЕ обхода: звонки к этому моменту уже сняты.
    using RingVisitor = std::function<void(const ExpiredRing &)>;
    void expireRinging(TimePoint now, const RingVisitor &visitor);

    // Кому пора платить за разговор: сторона-инициатор, у которой с прошлого списания
    // прошло CALL_CHARGE_INTERVAL. Отметку времени сервис двигает САМ, поэтому
    // визитор зовётся ровно один раз за интервал; списывает деньги система.
    struct CallCharge
    {
        int payerId = -1;
        std::uint32_t payerSerial = 0;
        int peerId = -1;
        std::uint32_t peerSerial = 0;
    };
    using ChargeVisitor = std::function<void(const CallCharge &)>;
    void collectCharges(TimePoint now, const ChargeVisitor &visitor);

    // --- заказы ---
    // Создать заказ. 0 — у звонящего уже есть живой заказ (анти-спам).
    int createOrder(Service service, int callerId, std::uint32_t callerSerial, const Vector3 &position,
                    TimePoint now);
    // Принять заказ: отдаёт копию и УДАЛЯЕТ его (первый принявший забирает вызов).
    // false — номера нет, он протух или это чужая служба.
    bool acceptOrder(int number, Service service, TimePoint now, Order &out);
    // Живой заказ звонящего (для отказа в повторном вызове). nullptr — нет.
    const Order *orderOfCaller(int callerId, TimePoint now) const;
    void cancelOrdersOf(int callerId); // выход/смерть звонящего

  private:
    // --- вызывается ТОЛЬКО PhoneSystem ---
    void setPhone(int playerId, std::int64_t phone);
    void setBalance(int playerId, std::int64_t balance); // клампится снизу нулём
    // Снять со счёта. false — не хватило, счёт НЕ тронут (в минус не уходим).
    bool takeBalance(int playerId, std::int64_t amount);
    // Полная очистка слота (номер, заказы, звонок, заявка на номер). Возвращает
    // peerId снятого звонка (-1 — звонка не было), чтобы система уведомила его.
    int resetPlayer(int playerId);
    // Заявка на номер уже ушла в БД: второй параллельной покупки быть не должно
    // (иначе двойное списание за один номер). false — заявка уже висит.
    bool beginNumberRequest(int playerId);
    void endNumberRequest(int playerId);

    struct Dispatch
    {
        std::string callName;
        DispatchEnumerator enumerate;
    };

    // Когда и на какой номер игрок звонил в последний раз (пауза повторного вызова).
    struct Recall
    {
        std::int64_t phone = 0;
        TimePoint at;
    };

    static bool validService(Service service);
    void dropExpired(TimePoint now);
    // Снять половину звонка и поддержать счётчик живых связок.
    void clearLink(int playerId);
    // Согласована ли пара: у собеседника звонок есть и он указывает на нас.
    bool paired(int playerId) const;

    std::array<Dispatch, static_cast<std::size_t>(Service::Count)> m_dispatch;
    std::array<std::int64_t, MAX_PLAYERS> m_phone{};   // NO_PHONE — телефона нет
    std::array<std::int64_t, MAX_PLAYERS> m_balance{}; // счёт телефона, никогда < 0
    std::unordered_map<int, Order> m_orders;         // номер заказа -> заказ
    int m_nextOrderNumber = 1000;

    std::array<CallLink, MAX_PLAYERS> m_calls;
    std::array<Recall, MAX_PLAYERS> m_recall;
    std::array<bool, MAX_PLAYERS> m_numberRequest{}; // покупка номера в полёте
    // Сколько половин звонков живо: пока их нет, тик дозвонов не обходит массив.
    int m_linkCount = 0;
};
