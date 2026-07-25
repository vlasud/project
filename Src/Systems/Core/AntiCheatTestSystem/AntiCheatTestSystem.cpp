#include "Systems/Core/AntiCheatTestSystem/AntiCheatTestSystem.h"

#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <Server/Components/Vehicles/vehicles.hpp>
#include <cmath>
#include <fmt/format.h>
#include <glm/trigonometric.hpp>

namespace
{
const Colour DEBUG_COLOUR{120, 220, 255};

// Развороты: шаг чуть длиннее интервала синка (33 мс), чтобы клиент успел
// применить угол и прислать его обратно. Шагов заметно больше порога серии.
constexpr Milliseconds TURN_STEP{80};
constexpr int TURN_STEPS = 14;

// Бег: 1.5 м каждые 100 мс — это 15 м/с, выше лимита скорости пешком, но в
// пределах лимита достижимости позиции (иначе поймает не тот детектор).
// Длительность с запасом над окном устойчивости.
constexpr Milliseconds RUN_STEP{100};
constexpr int RUN_STEPS = 40;
constexpr float RUN_DISTANCE = 1.5f;

// Рывок позиции: заметно дальше любого легального перемещения за тик.
constexpr float JUMP_DISTANCE = 500.0f;

// Оружие и броня для тестов: M4 в руках мимо инвентаря и полная броня мимо
// сервиса здоровья.
constexpr std::uint8_t TEST_WEAPON = 31;
constexpr std::uint32_t TEST_AMMO = 100;
constexpr std::uint32_t TEST_HACK_AMMO = 999; // заведомо выше любого серверного остатка
constexpr float TEST_ARMOUR = 100.0f;
constexpr float TEST_VEHICLE_HEALTH = 1500.0f;

// Направление «вперёд» по углу модели (SA-MP: 0 — север, дальше против часовой).
Vector3 facing(float yawDegrees)
{
    const float radians = glm::radians(yawDegrees);
    return Vector3{-std::sin(radians), std::cos(radians), 0.0f};
}
} // namespace

AntiCheatTestSystem::AntiCheatTestSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_timerService(serviceRegister.getService<TimerService>()),
      m_antiCheatService(serviceRegister.getService<AntiCheatService>()),
      m_weaponService(serviceRegister.getService<PlayerWeaponService>())
{
    serviceRegister.getService<PlayerCommandService>().add(
        "actest", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showMenu(player); },
        PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "панель проверки детекторов анти-чита",
        PlayerCommandService::HelpCategory::Hidden);
}

void AntiCheatTestSystem::notify(IPlayer &player, const std::string &text) const
{
    player.sendClientMessage(DEBUG_COLOUR, u(text));
}

void AntiCheatTestSystem::showMenu(IPlayer &player)
{
    const std::string body = "Мгновенные развороты\tQuickTurn\n"
                             "Бег 15 м/с (4 сек)\tSpeedHack\n"
                             "Рывок на 500 м\tTeleportHack\n"
                             "Броня мимо сервиса\tHealthHack\n"
                             "Оружие мимо инвентаря\tWeaponHack\n"
                             "Патроны мимо сервиса\tWeaponHack (ammo)\n"
                             "Джетпак без выдачи\tSpecialActionHack\n"
                             "Ремонт машины мимо сервиса\tVehicleHack";

    m_dialogService.show(
        player, makeDialog(DialogStyle_TABLIST, "Проверка анти-чита", body, "Запустить", "Закрыть"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;
            if (listItem < 0 || listItem >= static_cast<int>(Test::Count))
                return;
            run(*player, static_cast<Test>(listItem));
        });
}

void AntiCheatTestSystem::run(IPlayer &player, Test test)
{
    const int playerId = player.getID();

    if (m_running[playerId])
    {
        notify(player, "Предыдущий тест ещё идёт — дождитесь его конца");
        return;
    }

    // Журнал чистим перед каждым тестом: иначе записи прошлых прогонов доберут
    // порог кика и выбросят тестера на середине проверки.
    m_antiCheatService.clear(playerId);

    switch (test)
    {
    case Test::QuickTurn:
        m_running[playerId] = true;
        notify(player, "Разворачиваю модель рывками — ожидается QuickTurn");
        stepTurn(playerId, TURN_STEPS);
        return;

    case Test::SpeedHack:
        m_running[playerId] = true;
        notify(player, "Тащу вперёд со скоростью 15 м/с — ожидается SpeedHack");
        stepRun(playerId, RUN_STEPS);
        return;

    case Test::TeleportHack:
    {
        // Мимо PlayerLocationService: сервис как раз и отличает свой телепорт от
        // чужого рывка — второе должно быть поймано и откачено назад.
        const Vector3 target = player.getPosition() + facing(player.getRotation().ToEuler().z) * JUMP_DISTANCE;
        player.setPosition(target);
        notify(player, "Рывок на 500 метров — ожидается TeleportHack и откат назад");
        return;
    }

    case Test::HealthHack:
        // Мимо PlayerHealthService: сумма HP+броня вырастет без серверной санкции.
        player.setArmour(TEST_ARMOUR);
        notify(player, "Выдал 100 брони мимо сервиса — ожидается HealthHack и откат");
        return;

    case Test::WeaponHack:
        // Мимо PlayerWeaponService: оружие в руках, которого нет в серверном инвентаре.
        player.giveWeapon(WeaponSlotData{TEST_WEAPON, TEST_AMMO});
        player.setArmedWeapon(TEST_WEAPON);
        notify(player, "Выдал M4 мимо инвентаря — ожидается WeaponHack и снятие оружия");
        return;

    case Test::AmmoHack:
    {
        // Накрутка патронов тому стволу, который сервер реально выдавал: ловиться
        // должен именно рост остатка, а не появление чужого оружия.
        const std::uint8_t armed = m_weaponService.getArmedWeapon(playerId);
        if (armed == 0 || m_weaponService.getAmmo(playerId, armed) < 0)
        {
            notify(player, "Возьмите в руки оружие, выданное сервером (/agun), и повторите");
            return;
        }
        // Мимо PlayerWeaponService: у клиента патронов становится больше, чем знает
        // сервер, — ровно то, что делает ammo hack.
        player.setWeaponAmmo(WeaponSlotData{armed, TEST_HACK_AMMO});
        notify(player, fmt::format("Накрутил {} патронов оружию {} мимо сервиса — ожидается WeaponHack и откат",
                                   TEST_HACK_AMMO, armed));
        return;
    }

    case Test::Jetpack:
        // Мимо PlayerStateService: экшен без серверной выдачи.
        player.setAction(SpecialAction_Jetpack);
        notify(player, "Включил джетпак без выдачи — ожидается SpecialActionHack и снятие");
        return;

    case Test::VehicleRepair:
    {
        IPlayerVehicleData *data = queryExtension<IPlayerVehicleData>(player);
        IVehicle *vehicle = data ? data->getVehicle() : nullptr;
        if (!vehicle)
        {
            notify(player, "Сядьте за руль: тест поднимает HP машины мимо сервиса");
            return;
        }
        // Мимо VehicleService: рост HP машины без серверной санкции = repair hack.
        vehicle->setHealth(TEST_VEHICLE_HEALTH);
        notify(player, "Поднял HP машины мимо сервиса — ожидается VehicleHack");
        return;
    }

    case Test::Count:
        return;
    }
}

void AntiCheatTestSystem::stepTurn(int playerId, int stepsLeft)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        m_running[playerId] = false;
        return;
    }
    if (stepsLeft <= 0)
    {
        finish(playerId);
        return;
    }

    // Разворот кругом: клиент применит угол и вернёт его следующим синком — для
    // детектора это неотличимо от мгновенного разворота читом.
    const float yaw = player->getRotation().ToEuler().z;
    player->setRotation(GTAQuat(Vector3(0.0f, 0.0f, yaw + 180.0f)));

    m_timerService.setTimeout(TURN_STEP, [this, playerId, stepsLeft]() { stepTurn(playerId, stepsLeft - 1); });
}

void AntiCheatTestSystem::stepRun(int playerId, int stepsLeft)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        m_running[playerId] = false;
        return;
    }
    if (stepsLeft <= 0)
    {
        finish(playerId);
        return;
    }

    // Сдвиг мимо сервиса позиции: каждый шаг в пределах достижимого за тик (чтобы
    // сработал детектор скорости, а не телепорта), но темп держится выше лимита.
    const Vector3 position = player->getPosition() + facing(player->getRotation().ToEuler().z) * RUN_DISTANCE;
    player->setPosition(position);

    m_timerService.setTimeout(RUN_STEP, [this, playerId, stepsLeft]() { stepRun(playerId, stepsLeft - 1); });
}

void AntiCheatTestSystem::finish(int playerId)
{
    m_running[playerId] = false;
    if (IPlayer *player = m_core.getPlayers().get(playerId))
    {
        notify(*player, "Тест закончен — смотрите /violations");
    }
}
