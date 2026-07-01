#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "types.hpp"
#include <Server/Components/Pickups/pickups.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>

class PickupSystem;
class StreamerService;
class PlayerLocationService;
class AntiCheatService;

// Сервис интерактивных пикапов: создание (через стример — без клиентских
// лимитов) + маршрутизация подбора с валидацией.
//
//   int id = m_pickups.add(1212, 2, {x, y, z}, [](IPlayer &player) {
//       // игрок подобрал пикап (уже проверено: дистанция, мир, антиспам)
//   });
//   m_pickups.remove(id); // можно прямо из обработчика (одноразовый пикап)
//
// Валидация подбора (клиент может прислать RPC подбора для любого
// застримленного пикапа в любой момент):
//  * дистанция — принятая сервером позиция игрока должна быть рядом с пикапом
//    (допуск покрывает лаг и проезд на скорости), иначе фиксируется PickupHack;
//  * виртуальный мир игрока должен совпадать с миром пикапа;
//  * edge-триггер — клиент шлёт RPC подбора ПОВТОРНО, пока игрок стоит на
//    пикапе (open.mp — чистый релей, не троттлит: pickups_main.cpp onReceive->
//    dispatch). Обработчик зовём РАЗОВО на переходе «не на пикапе»->«на пикапе»:
//    непрерывный поток событий держит «игрок на пикапе», повтор только если
//    поток прерывался дольше REARM_GAP (игрок ушёл и вернулся). Иначе диалоги
//    (парковка/дом) переоткрываются каждую секунду, а пикапы-предметы дюпят
//    эффект.
class PickupService final : public IService
{
    friend PickupSystem;

  public:
    using Handler = std::function<void(IPlayer &)>;

    // Допуск дистанции подбора: сам подбор срабатывает на клиенте в ~1 м, но
    // принятая сервером позиция отстаёт от клиентской на лаг (на машине на
    // скорости — заметно), поэтому порог с запасом.
    static constexpr float MAX_PICKUP_DISTANCE = 15.0f;

    // Разрыв в потоке событий подбора, после которого игрок считается сошедшим
    // с пикапа — следующее событие снова сработает как вход. Клиент шлёт RPC
    // подбора часто (кадрово, ~несколько раз в секунду), пока игрок стоит на
    // пикапе; порог с большим запасом над этим интервалом, чтобы дрожание/лаг
    // не переоткрывали handler, но сход и возврат ре-армили его. Ошибка в
    // сторону «не переоткрыть» безопаснее флуда.
    static constexpr Milliseconds DEFAULT_REARM_GAP{2000};

    // Создать пикап с обработчиком подбора. model — модель объекта,
    // type — клиентский тип поведения пикапа (SA), rearmGap — разрыв в потоке
    // событий, после которого вход на пикап снова сработает (см. выше).
    // Возвращает id пикапа или -1.
    int add(int model, PickupType type, const Vector3 &position, Handler onPickUp, std::uint32_t virtualWorld = 0,
            Milliseconds rearmGap = DEFAULT_REARM_GAP);
    void remove(int pickupId);
    bool exists(int pickupId) const;

  private:
    struct Def
    {
        Vector3 position{};
        std::uint32_t virtualWorld = 0;
        Milliseconds rearmGap{DEFAULT_REARM_GAP};
        int model = 0;
        Handler handler;
    };

    // Вызываются PickupSystem.
    void initialize(StreamerService *streamer, PlayerLocationService *location, AntiCheatService *antiCheat);
    void handlePickUp(IPlayer &player, IPickup &pickup, TimePoint now);
    void resetPlayer(int playerId);

    StreamerService *m_streamer = nullptr;
    PlayerLocationService *m_location = nullptr;
    AntiCheatService *m_antiCheat = nullptr;

    std::unordered_map<int, Def> m_defs;                                      // ключ — def id стримера
    std::array<std::unordered_map<int, TimePoint>, MAX_PLAYERS> m_lastPickUp; // время последнего события per-player per-def
};
