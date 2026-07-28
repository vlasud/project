#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>
#include <functional>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class TaxiJobSystem;

// Работа-таксист — бизнес-фича (НЕ Core). Источник правды о:
//  * пер-player фазе смены (не работает / в очереди / машина выдана, идёт выезд /
//    на смене);
//  * занятости SPOT_COUNT точек спавна такси и FIFO-очереди;
//  * ПОЕЗДКЕ: кто пассажир, куда едем, о какой сумме договорились и внесена ли она.
//
// Депо-модель — как у врача: pre-stock машин нет, точка пустует до очереди, а
// «выезд» считается по факту ОТЪЕЗДА с точки (иначе таксист занимал бы её всю смену).
//
// ДЕНЬГИ ПОЕЗДКИ — ДЕПОЗИТ. Согласованная сумма списывается с пассажира сразу, но
// водителю не отдаётся: она лежит в записи поездки, пока такси не приедет на точку
// назначения. Любой другой конец поездки (водитель вышел/погиб/уволился, пассажир
// вышел из машины или из игры, машина пропала) обязан ВЕРНУТЬ депозит пассажиру —
// поэтому endRide отдаёт вызывающему всё, что нужно для возврата, и очищает запись
// одним действием: забыть про висящий депозит нельзя.
class TaxiJobService final : public IService
{
    friend TaxiJobSystem;

  public:
    TaxiJobService();

    // Точки спавна такси у таксопарка (замеры владельца, координаты — в приводе).
    static constexpr int SPOT_COUNT = 7;

    enum class Phase
    {
        NotWorking, // не в смене
        Queued,     // устроен, ждёт свободную точку спавна в FIFO-очереди
        Boarding,   // такси подано на точку: сесть и ОТЪЕХАТЬ, пока идёт окно
        Working     // на смене: машина своя, точка спавна освобождена
    };

    // Поездка водителя. passengerId < 0 — пассажира нет, остальные поля не значат
    // ничего.
    struct Ride
    {
        int passengerId = -1;
        bool hasDestination = false;
        Vector3 destination{};
        std::string destinationName; // utf-8, для сообщений («метка на карте» и т.п.)
        std::int64_t fare = 0;       // предложенная (offered) или внесённая (paid) сумма
        bool paid = false;           // деньги списаны с пассажира и лежат в депозите
    };

    Phase phaseOf(int playerId) const;
    bool isWorking(int playerId) const; // phase != NotWorking
    int vehicleIdOf(int playerId) const;
    int queuePositionOf(int playerId) const; // 1-based место; 0 — не в очереди
    std::vector<int> queuedPlayers() const;

    // --- точки спавна ---
    int spotOf(int playerId) const;
    int holderOfSpot(int spot) const;
    int workerOfVehicle(int vehicleId) const; // гейт водителя; -1 — не наша машина

    // --- поездка ---
    const Ride *rideOf(int driverId) const;      // nullptr — водитель не в смене
    int driverOfPassenger(int passengerId) const; // водитель, который везёт игрока; -1 — никто

  private:
    // --- вызывается ТОЛЬКО TaxiJobSystem ---

    enum class StartResult
    {
        AlreadyWorking,
        Boarding,
        Queued
    };
    struct StartOutcome
    {
        StartResult result;
        int spot = -1;
        int queuePosition = 0;
    };

    // usable(spot) — «на точке физически свободно» (привод спрашивает мир: чужая
    // машина могла встать на место, и спавн внутрь неё — то, что видит игрок).
    // Сервис о мире не знает, поэтому проверку приносит вызывающий.
    using SpotUsable = std::function<bool(int spot)>;

    StartOutcome startWork(int playerId, const SpotUsable &usable);
    void setVehicle(int playerId, int vehicleId);
    void completeBoarding(int playerId); // машина отъехала: Boarding -> Working
    void requeueTail(int playerId);      // провал выезда: в конец очереди

    struct Promotion
    {
        int playerId = -1;
        int spot = -1;
    };
    Promotion promoteQueue(const SpotUsable &usable);

    // --- поездка (мутаторы) ---
    // Посадить пассажира. Отказ (false), если водитель не на смене или пассажир уже
    // есть: вторая поездка поверх первой потеряла бы депозит.
    bool setPassenger(int driverId, int passengerId);
    // Назначить точку назначения (выбор пассажира). Сбрасывает НЕ внесённую цену:
    // договаривались за другой маршрут. Внесённый депозит не трогает.
    bool setDestination(int driverId, const Vector3 &destination, std::string name);
    // Предложить сумму (водитель). Только при живой поездке с назначенной точкой и
    // ещё не внесённым депозитом.
    bool offerFare(int driverId, std::int64_t fare);
    // Пассажир согласился и деньги списаны: депозит внесён.
    bool confirmFare(int driverId);

    // Закрыть поездку и отдать её последнее состояние вызывающему (возврат депозита
    // и сообщения — на приводе). После вызова запись пуста.
    Ride endRide(int driverId);

    // Завершить смену: снять точку, очередь, машину. Поездку НЕ закрывает — привод
    // обязан закрыть её ДО этого (иначе депозит потеряется молча).
    void endShift(int playerId);

    struct State
    {
        Phase phase = Phase::NotWorking;
        int vehicleId = -1;
        int spot = -1;
        Ride ride;
    };

    int firstFreeSpot(const SpotUsable &usable) const;
    void removeFromQueue(int playerId);
    void releaseSpot(int playerId);

    std::array<State, MAX_PLAYERS> m_state{};
    std::array<int, SPOT_COUNT> m_spotHolder{};
    std::deque<int> m_queue;
};
