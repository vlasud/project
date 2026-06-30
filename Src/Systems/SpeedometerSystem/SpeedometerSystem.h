#pragma once

#include "Macro.h"
#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>

// HUD за рулём — приборка водителя: скорость (км/ч), HP машины и топливо.
// Три per-player textdraw, видны пока игрок за рулём, обновление раз в 0.5 с.
// Бизнес-фича (не Core).
//
// Скорость — КЛИЕНТСКАЯ велосити машины (VehicleService::getVelocity, то, что
// водитель заявил в driver sync), отзывчивее серверной. Это безопасно: HUD
// КОСМЕТИЧЕСКИЙ — рисуется только самому водителю, никакая логика/анти-чит от
// него не зависят (источник правды о скорости — PlayerVelocityService).
// HP и топливо — из VehicleService (источник правды о машине).
class SpeedometerSystem : public BaseSystem, public PlayerChangeEventHandler, public PlayerConnectEventHandler
{
  public:
    SpeedometerSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerStateChange(IPlayer &player, PlayerState newState, PlayerState oldState) override;
    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    // Показать HUD водителю: лениво создать per-player textdraw'ы, show() и сразу
    // обновить значения.
    void showFor(IPlayer &player);
    // Скрыть HUD: hide() всех элементов и снять флаг вождения. Берёт IPlayer из пула.
    void hideFor(int playerId);
    // Глобальный 0.5-с тик: обновить значения у всех водителей с активным HUD.
    void tick();
    // Вычислить и записать скорость/HP/топливо в textdraw'ы игрока.
    void updateHud(IPlayer &player);
    // Лениво создать (если ещё нет) и показать один элемент HUD; nullptr при
    // исчерпании пула per-player текстдравов.
    IPlayerTextDraw *ensureShown(IPlayer &player, int &id, const Vector2 &position, const TextDrawParams &params);

    struct Hud
    {
        int speedId = -1;       // textdraw скорости (крупное число)
        int hpId = -1;          // textdraw HP машины
        int fuelId = -1;        // textdraw топлива
        bool driving = false;   // HUD сейчас показан (игрок за рулём)
        bool speedDimmed = false; // число скорости приглушено (двигатель заглушён)
    };

    TextDrawService &m_textDrawService;
    VehicleService &m_vehicleService;
    TimerService &m_timerService;

    std::array<Hud, MAX_PLAYERS> m_huds;
};
