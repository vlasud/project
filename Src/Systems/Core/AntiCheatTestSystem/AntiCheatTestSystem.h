#pragma once

#include "Macro.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerVelocityService/PlayerVelocityService.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <cstdint>

// Дев-панель проверки детекторов анти-чита: /actest открывает меню, каждый пункт
// воспроизводит поведение соответствующего чита, после чего нарушение должно
// появиться в /violations. Нужна, чтобы отличать «детект молчит, потому что всё
// чисто» от «детект молчит, потому что сломан» — без модифицированного клиента.
//
// НАМЕРЕННЫЙ ОБХОД СЕРВИСОВ: пункты дёргают сырой SDK (setArmour, giveWeapon,
// setAction, setPosition, setRotation) МИМО PlayerHealthService/PlayerWeaponService/
// PlayerStateService/PlayerLocationService. Это не нарушение конвенции, а суть
// инструмента: ровно так же состояние меняет чит, и именно это должны увидеть
// валидаторы. В игровой логике так делать по-прежнему нельзя.
//
// Чего панель проверить НЕ может: SilentAim, CarShot, RapidFire, DamageHack. Они
// висят на пакетах, которые шлёт клиент (bullet sync, give-damage) — сервер не может
// заставить свой клиент их отправить. Для них нужен модифицированный клиент.
class AntiCheatTestSystem : public BaseSystem, public PlayerUpdateEventHandler
{
  public:
    AntiCheatTestSystem(ICore &core, const ServiceRegister &serviceRegister);

    // Разгон двигает позицию ЗДЕСЬ, на приходе синка, а не по своему таймеру:
    // сдвиг считается от только что принятой клиентской точки, поэтому клиент его
    // накапливает. Асинхронные телепорты клиент, наоборот, отбивал своей старой
    // позицией, и серверная скорость не росла.
    bool onPlayerUpdate(IPlayer &player, TimePoint now) override;

  private:
    // Пункты корневого меню (порядок совпадает с порядком строк).
    enum class MenuItem : std::uint8_t
    {
        Tests,
        State,
        Weights,
        Threshold,
        KickedMultiplier,
        OwnMultiplier,
        NoAdminMultiplier,
        ResetScore,
        ResetTuning,
        Count,
    };

    // Порядок совпадает с порядком пунктов меню симуляций.
    enum class Test : std::uint8_t
    {
        QuickTurn,
        SpeedHack,
        TeleportHack,
        HealthHack,
        WeaponHack,
        AmmoHack,
        Jetpack,
        VehicleRepair,
        Count,
    };

    void showMenu(IPlayer &player);      // корневое меню панели
    void showTests(IPlayer &player);     // список симуляций
    void showState(IPlayer &player);     // счёт, множитель, порог
    void showWeights(IPlayer &player);   // список весов по типам нарушений
    void editWeight(IPlayer &player, AntiCheatService::ViolationType type);
    void editThreshold(IPlayer &player);
    void editKickedMultiplier(IPlayer &player);
    void editOwnMultiplier(IPlayer &player);
    void editNoAdminMultiplier(IPlayer &player);
    void run(IPlayer &player, Test test);

    // Развороты — цепочка: каждый шаг планирует следующий через setTimeout, пока не
    // кончится счётчик. Так тест не держит таймер, который пришлось бы отменять.
    void stepTurn(int playerId, int stepsLeft);
    void finish(int playerId);

    // Состояние разгона: живёт до истечения until, шаг делается на каждом синке.
    struct SpeedTest
    {
        TimePoint until;      // до какого момента тащить
        TimePoint lastPush;   // прошлый сдвиг — для расчёта дистанции по dt
        TimePoint lastReport; // прошлый показ серверной скорости
    };
    std::array<SpeedTest, MAX_PLAYERS> m_speed{};

    void notify(IPlayer &player, const std::string &text) const;

    PlayerDialogService &m_dialogService;
    TimerService &m_timerService;
    AntiCheatService &m_antiCheatService;
    // Только чтение принятого оружия в руках — тест патронов накручивает их мимо
    // сервиса, но накручивать нужно именно тому стволу, который сервер выдал.
    PlayerWeaponService &m_weaponService;
    // Только чтение серверной скорости: тест ускорителя показывает её игроку, чтобы
    // отличать «детект не сработал» от «симуляция не разогнала».
    PlayerVelocityService &m_velocityService;

    // Один тест-цепочка на игрока: повторный запуск до финиша только путал бы
    // картину в журнале.
    std::array<bool, MAX_PLAYERS> m_running{};
};
