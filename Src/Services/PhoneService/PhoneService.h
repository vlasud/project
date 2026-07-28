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

// Телефон — бизнес-сервис (НЕ Core). Держит две вещи:
//
//  * НОМЕРА ТЕЛЕФОНОВ игроков онлайн (кэш; персистятся в player.phone). Номер
//    выдаётся аккаунту один раз и дальше живёт с ним.
//  * ВЫЗОВЫ СЛУЖБ: заказ такси/скорой/полиции с уникальным номером, который
//    работник принимает командой.
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
    std::int64_t phoneOf(int playerId) const;    // NO_PHONE — номер ещё не загружен
    int playerByPhone(std::int64_t phone) const; // -1 — такого номера нет в сети

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
    void resetPlayer(int playerId);

    struct Dispatch
    {
        std::string callName;
        DispatchEnumerator enumerate;
    };

    static bool validService(Service service);
    void dropExpired(TimePoint now);

    std::array<Dispatch, static_cast<std::size_t>(Service::Count)> m_dispatch;
    std::array<std::int64_t, MAX_PLAYERS> m_phone{}; // NO_PHONE — не загружен
    std::unordered_map<int, Order> m_orders;         // номер заказа -> заказ
    int m_nextOrderNumber = 1000;
};
