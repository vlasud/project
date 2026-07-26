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

// Разгон. Два способа, которые НЕ работают, и почему:
//  * setVelocity пешему — игра гасит импульс до скорости бега, сервер видит 5-7 м/с;
//  * телепорты по своему таймеру — клиент не успевает их применять и шлёт назад
//    старую точку, дельта позиции пилит около нуля.
// Работает третий: сдвигать позицию на приходе синка, отсчитывая от только что
// принятой клиентской точки — такой сдвиг клиент накапливает.
constexpr float SPEED_TARGET = 16.0f; // м/с при лимите 9
constexpr Seconds SPEED_DURATION{5};  // вдвое больше окна устойчивости
constexpr Milliseconds SPEED_REPORT{500};
// Ограничение шага: при разрыве синков (лаг, пауза) dt большой, и один сдвиг стал
// бы прыжком через полкарты — его поймал бы детектор телепорта вместо скорости.
constexpr float SPEED_MAX_STEP = 1.5f;

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
      m_weaponService(serviceRegister.getService<PlayerWeaponService>()),
      m_velocityService(serviceRegister.getService<PlayerVelocityService>())
{
    // Разгон работает на приходе синка — подписка обязательна.
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);

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
    const std::string body =
        fmt::format("Симуляции читов\t{} шт.\n"
                    "Состояние: баллы и множитель\t{:.2f} / {:.2f}\n"
                    "Веса нарушений\tнастроить\n"
                    "Порог отключения\t{:.2f}\n"
                    "Множитель после кика\t{:.2f}\n"
                    "Мой множитель\t{:.2f}\n"
                    "Множитель без админов\t{:.2f} ({})\n"
                    "Сбросить счёт и журнал\t-\n"
                    "Сбросить настройки к дефолтам\t-",
                    static_cast<int>(Test::Count), m_antiCheatService.score(player.getID()),
                    m_antiCheatService.threshold(), m_antiCheatService.threshold(),
                    m_antiCheatService.kickedMultiplier(), m_antiCheatService.multiplier(player.getID()),
                    m_antiCheatService.noAdminMultiplier(),
                    m_antiCheatService.aggressive() ? "активен" : "спит");

    m_dialogService.show(
        player, makeDialog(DialogStyle_TABLIST, "Анти-чит: панель разработчика", body, "Выбрать", "Закрыть"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;
            if (listItem < 0 || listItem >= static_cast<int>(MenuItem::Count))
                return;

            switch (static_cast<MenuItem>(listItem))
            {
            case MenuItem::Tests:
                showTests(*player);
                return;
            case MenuItem::State:
                showState(*player);
                return;
            case MenuItem::Weights:
                showWeights(*player);
                return;
            case MenuItem::Threshold:
                editThreshold(*player);
                return;
            case MenuItem::KickedMultiplier:
                editKickedMultiplier(*player);
                return;
            case MenuItem::OwnMultiplier:
                editOwnMultiplier(*player);
                return;
            case MenuItem::NoAdminMultiplier:
                editNoAdminMultiplier(*player);
                return;
            case MenuItem::ResetScore:
                m_antiCheatService.clear(playerId);
                notify(*player, "Счёт и журнал очищены (множитель не тронут)");
                showMenu(*player);
                return;
            case MenuItem::ResetTuning:
                m_antiCheatService.resetTuning();
                notify(*player, "Веса, порог и множитель после кика возвращены к дефолтам кода");
                showMenu(*player);
                return;
            case MenuItem::Count:
                return;
            }
        });
}

void AntiCheatTestSystem::showTests(IPlayer &player)
{
    const std::string body = "Мгновенные развороты\tQuickTurn\n"
                             "Бег 16 м/с (5 сек)\tSpeedHack\n"
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

void AntiCheatTestSystem::showState(IPlayer &player)
{
    const int playerId = player.getID();
    const AntiCheatService::PlayerRecord &record = m_antiCheatService.get(playerId);
    const float threshold = m_antiCheatService.threshold();

    notify(player, fmt::format("Счёт сессии: {:.2f} из {:.2f} — до отключения {:.2f}", record.score, threshold,
                               threshold > record.score ? threshold - record.score : 0.0f));
    notify(player, fmt::format("Ваш множитель: {:.2f} (после кика ставится {:.2f}, хранится в БД)", record.multiplier,
                               m_antiCheatService.kickedMultiplier()));
    notify(player, fmt::format("Режим без админов: {} (надбавка {:.2f})",
                               m_antiCheatService.aggressive() ? "АКТИВЕН" : "спит",
                               m_antiCheatService.noAdminMultiplier()));
    notify(player, fmt::format("Нарушений за сессию: {}", record.total));
    for (std::size_t i = record.recent.size(); i > 0 && i > record.recent.size() - 3; --i)
    {
        const AntiCheatService::Violation &violation = record.recent[i - 1];
        notify(player, fmt::format("  [{}] +{:.2f} — {}", AntiCheatService::name(violation.type),
                                   m_antiCheatService.weight(violation.type) * record.multiplier, violation.detail));
    }
}

void AntiCheatTestSystem::showWeights(IPlayer &player)
{
    std::string body;
    for (std::size_t i = 0; i < static_cast<std::size_t>(AntiCheatService::ViolationType::Count); ++i)
    {
        const auto type = static_cast<AntiCheatService::ViolationType>(i);
        const float weight = m_antiCheatService.weight(type);
        // Сколько таких событий отключает игрока — то, ради чего вес и крутят.
        const int events = weight > 0.0f ? static_cast<int>(std::ceil(m_antiCheatService.threshold() / weight)) : 0;
        body += fmt::format("{}\t{:.2f}\t{} шт. до кика\n", AntiCheatService::name(type), weight, events);
    }

    m_dialogService.show(player,
                         makeDialog(DialogStyle_TABLIST, "Веса нарушений", body, "Изменить", "Назад"),
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                                 return;
                             if (response != DialogResponse_Left)
                             {
                                 showMenu(*player);
                                 return;
                             }
                             if (listItem < 0 ||
                                 listItem >= static_cast<int>(AntiCheatService::ViolationType::Count))
                                 return;
                             editWeight(*player, static_cast<AntiCheatService::ViolationType>(listItem));
                         });
}

void AntiCheatTestSystem::editWeight(IPlayer &player, AntiCheatService::ViolationType type)
{
    // Значения вводятся в СОТЫХ: диалог ввода отдаёт целое, а веса дробные.
    const std::string body = fmt::format("Вес нарушения {}\nСейчас: {:.2f}\n\nВведите новое значение в сотых\n"
                                         "(100 = 1.00 — одно событие отключает; 25 = 0.25 — четыре события)",
                                         AntiCheatService::name(type), m_antiCheatService.weight(type));

    m_dialogService.showNumberInput(
        player, makeDialog(DialogStyle_INPUT, "Вес нарушения", body, "Применить", "Назад"),
        [this, playerId = player.getID(), type](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response == DialogResponse_Left)
            {
                m_antiCheatService.setWeight(type, static_cast<float>(value) / 100.0f);
                notify(*player, fmt::format("Вес {} = {:.2f}", AntiCheatService::name(type),
                                            m_antiCheatService.weight(type)));
            }
            showWeights(*player);
        });
}

void AntiCheatTestSystem::editThreshold(IPlayer &player)
{
    const std::string body = fmt::format("Порог отключения\nСейчас: {:.2f}\n\nВведите значение в сотых\n"
                                         "(100 = 1.00 — стандартный порог)",
                                         m_antiCheatService.threshold());

    m_dialogService.showNumberInput(player,
                                    makeDialog(DialogStyle_INPUT, "Порог отключения", body, "Применить", "Назад"),
                                    [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
                                    {
                                        IPlayer *player = m_core.getPlayers().get(playerId);
                                        if (!player)
                                            return;
                                        if (response == DialogResponse_Left)
                                        {
                                            m_antiCheatService.setThreshold(static_cast<float>(value) / 100.0f);
                                            notify(*player, fmt::format("Порог = {:.2f}",
                                                                        m_antiCheatService.threshold()));
                                        }
                                        showMenu(*player);
                                    });
}

void AntiCheatTestSystem::editKickedMultiplier(IPlayer &player)
{
    const std::string body = fmt::format("Множитель, который получает пойманный\nСейчас: {:.2f}\n\n"
                                         "Введите значение в сотых (120 = 1.20)",
                                         m_antiCheatService.kickedMultiplier());

    m_dialogService.showNumberInput(player,
                                    makeDialog(DialogStyle_INPUT, "Множитель после кика", body, "Применить", "Назад"),
                                    [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
                                    {
                                        IPlayer *player = m_core.getPlayers().get(playerId);
                                        if (!player)
                                            return;
                                        if (response == DialogResponse_Left)
                                        {
                                            m_antiCheatService.setKickedMultiplier(static_cast<float>(value) / 100.0f);
                                            notify(*player, fmt::format("Множитель после кика = {:.2f}",
                                                                        m_antiCheatService.kickedMultiplier()));
                                        }
                                        showMenu(*player);
                                    });
}

void AntiCheatTestSystem::editOwnMultiplier(IPlayer &player)
{
    const std::string body = fmt::format("Ваш личный множитель\nСейчас: {:.2f}\n\n"
                                         "Введите значение в сотых (100 = 1.00, 120 = 1.20).\n"
                                         "Правка только в памяти сессии: в БД уходит\n"
                                         "лишь подъём после реального кика.",
                                         m_antiCheatService.multiplier(player.getID()));

    m_dialogService.showNumberInput(player,
                                    makeDialog(DialogStyle_INPUT, "Мой множитель", body, "Применить", "Назад"),
                                    [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
                                    {
                                        IPlayer *player = m_core.getPlayers().get(playerId);
                                        if (!player)
                                            return;
                                        if (response == DialogResponse_Left)
                                        {
                                            m_antiCheatService.setMultiplier(playerId, static_cast<float>(value) / 100.0f);
                                            notify(*player, fmt::format("Ваш множитель = {:.2f}",
                                                                        m_antiCheatService.multiplier(playerId)));
                                        }
                                        showMenu(*player);
                                    });
}

void AntiCheatTestSystem::editNoAdminMultiplier(IPlayer &player)
{
    const std::string body =
        fmt::format("Надбавка, пока онлайн нет залогиненных админов\nСейчас: {:.2f} ({})\n\n"
                    "Введите значение в сотых (120 = 1.20, 100 = выключить).\n"
                    "Считается в момент нарушения: зашёл админ — надбавка снята.",
                    m_antiCheatService.noAdminMultiplier(), m_antiCheatService.aggressive() ? "активна" : "спит");

    m_dialogService.showNumberInput(
        player, makeDialog(DialogStyle_INPUT, "Множитель без админов", body, "Применить", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response == DialogResponse_Left)
            {
                m_antiCheatService.setNoAdminMultiplier(static_cast<float>(value) / 100.0f);
                notify(*player,
                       fmt::format("Множитель без админов = {:.2f}", m_antiCheatService.noAdminMultiplier()));
            }
            showMenu(*player);
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

    // Журнал НЕ чистим: счёт должен копиться от теста к тесту, иначе порог кика
    // не проверить. Обнуление — отдельным пунктом меню.

    switch (test)
    {
    case Test::QuickTurn:
        m_running[playerId] = true;
        notify(player, "Разворачиваю модель рывками — ожидается QuickTurn");
        stepTurn(playerId, TURN_STEPS);
        return;

    case Test::SpeedHack:
        // В транспорте лимит скорости другой (120 м/с) — тест пешего ускорителя
        // там просто ничего не покажет.
        if (player.getState() != PlayerState_OnFoot)
        {
            notify(player, "Выйдите из транспорта: тест проверяет пеший лимит скорости");
            return;
        }
    {
        m_running[playerId] = true;
        const TimePoint now = std::chrono::steady_clock::now();
        SpeedTest &test = m_speed[playerId];
        test.until = now + SPEED_DURATION;
        test.lastPush = now;
        test.lastReport = now;
        notify(player, "Тащу вперёд со скоростью 16 м/с (около 80 метров) — ожидается SpeedHack");
        return;
    }

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

bool AntiCheatTestSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    const int playerId = player.getID();
    SpeedTest &test = m_speed[playerId];
    if (test.until.time_since_epoch().count() == 0)
    {
        return true; // разгон не запускали
    }

    if (now >= test.until)
    {
        test = SpeedTest{};
        finish(playerId);
        return true;
    }

    // Шаг = целевая скорость * фактический интервал между синками: так серверная
    // скорость (дельта принятых позиций) выходит ровно на SPEED_TARGET.
    const float dt = std::chrono::duration<float>(now - test.lastPush).count();
    test.lastPush = now;
    const float step = std::min(SPEED_TARGET * dt, SPEED_MAX_STEP);
    player.setPosition(player.getPosition() + facing(player.getRotation().ToEuler().z) * step);

    // Диагностика: раз в полсекунды показываем, что видит СЕРВЕР. Без неё
    // «нарушения нет» неотличимо от «детект сломан» — как и вышло в первый раз.
    if (now - test.lastReport >= SPEED_REPORT)
    {
        test.lastReport = now;
        // Показываем всё, от чего зависит вердикт: скорость, вертикаль (уводит в ветку
        // падения), выбранную ветку лимитов и накопленное время над лимитом. По этим
        // четырём числам видно, где именно теряется нарушение.
        notify(player, fmt::format("{:.1f} м/с | вертикаль {:+.1f} | ветка: {} | над лимитом: {} мс (нужно 2000)",
                                   m_velocityService.getHorizontalSpeed(playerId),
                                   m_velocityService.getVerticalSpeed(playerId),
                                   m_velocityService.lastBranch(playerId), m_velocityService.overMs(playerId)));
    }
    return true;
}

void AntiCheatTestSystem::finish(int playerId)
{
    m_running[playerId] = false;
    if (IPlayer *player = m_core.getPlayers().get(playerId))
    {
        notify(*player, "Тест закончен — смотрите /violations");
    }
}
