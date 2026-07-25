#pragma once

#include "Macro.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
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
class AntiCheatTestSystem : public BaseSystem
{
  public:
    AntiCheatTestSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Порядок совпадает с порядком пунктов меню.
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

    void showMenu(IPlayer &player);
    void run(IPlayer &player, Test test);

    // Цепочки шагов: каждый шаг планирует следующий через setTimeout, пока не
    // кончится счётчик. Так тест не держит таймер, который пришлось бы отменять.
    void stepTurn(int playerId, int stepsLeft);
    void stepRun(int playerId, int stepsLeft);
    void finish(int playerId);

    void notify(IPlayer &player, const std::string &text) const;

    PlayerDialogService &m_dialogService;
    TimerService &m_timerService;
    AntiCheatService &m_antiCheatService;
    // Только чтение принятого оружия в руках — тест патронов накручивает их мимо
    // сервиса, но накручивать нужно именно тому стволу, который сервер выдал.
    PlayerWeaponService &m_weaponService;

    // Один тест-цепочка на игрока: повторный запуск до финиша только путал бы
    // картину в журнале.
    std::array<bool, MAX_PLAYERS> m_running{};
};
