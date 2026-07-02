#include "Systems/SpeedometerSystem/SpeedometerSystem.h"

#include "Log/LogManager.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fmt/format.h>

namespace
{
// Темп обновления HUD: достаточно для ощущения скорости, не дёргает младший
// разряд на крупном кегле.
constexpr std::chrono::milliseconds UPDATE_INTERVAL{500};

// Велосити машины (SA-юниты) -> км/ч. Стандартный множитель SA-MP:
// speed_kmh = |velocity| * 179.28 (не 3.6 — это уже не реальные м/с).
constexpr float SA_VELOCITY_TO_KMH = 179.28f;

// Экранные позиции элементов HUD: центр по X (320), низ экрана, стопкой.
const Vector2 SPEED_POS(320.0f, 392.0f); // крупное число скорости
const Vector2 HP_POS(320.0f, 430.0f);    // HP машины (мелкая строка)
const Vector2 FUEL_POS(320.0f, 438.0f);  // топливо (мелкая строка)

// Цвет числа скорости: непрозрачный белый при работающем двигателе, alpha 0x50
// при заглушённом (визуальный сигнал «мотор не работает»).
const Colour SPEED_COLOUR_ON = Colour::FromRGBA(0xFFFFFFFF);
const Colour SPEED_COLOUR_OFF = Colour::FromRGBA(0xFFFFFF50);

// Крупное центрированное число скорости, шрифт 3.
TextDrawParams speedParams()
{
    TextDrawParams params;
    params.alignment = TextDrawAlignment_Center;
    params.style = TextDrawStyle_3;
    params.letterSize = Vector2(0.5062f, 4.4297f);
    params.textSize = Vector2(400.0f, 17.0f);
    params.letterColour = SPEED_COLOUR_ON;
    params.box = false;
    params.boxColour = Colour::FromRGBA(0x00000080);
    params.backgroundColour = Colour::FromRGBA(0x000000FF);
    params.proportional = true;
    params.selectable = false;
    params.shadow = 0;
    params.outline = 0;
    return params;
}

// Мелкая центрированная строка (HP, топливо) под спидометром — тот же стиль,
// меньший кегль.
TextDrawParams lineParams()
{
    TextDrawParams params = speedParams();
    params.letterSize = Vector2(0.1027f, 0.8993f);
    return params;
}
} // namespace

SpeedometerSystem::SpeedometerSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_textDrawService(serviceRegister.getService<TextDrawService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_timerService(serviceRegister.getService<TimerService>())
{
    core.getPlayers().getPlayerChangeDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void SpeedometerSystem::initialize(IComponentList *components)
{
    // Без компонента textdraw HUD не рисуем — таймер не ставим.
    if (!m_textDrawService.isAvailable())
    {
        LogManager::log(Warning, "SpeedometerSystem: textdraw component is unavailable, HUD disabled");
        return;
    }

    // Глобальный 0.5-с тик на главном потоке: апдейтит только активные HUD.
    m_timerService.setInterval(UPDATE_INTERVAL, [this] { tick(); });
}

void SpeedometerSystem::onPlayerStateChange(IPlayer &player, PlayerState newState, PlayerState oldState)
{
    if (newState == PlayerState_Driver)
    {
        showFor(player);
    }
    else
    {
        hideFor(player.getID());
    }
}

void SpeedometerSystem::onPlayerConnect(IPlayer &player)
{
    // Чистый старт сессии: слот мог остаться грязным, если дисконнект прежнего
    // владельца не пришёл (open.mp не всегда шлёт его на штатной остановке).
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_huds[playerId] = {};
}

void SpeedometerSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    // Per-player textdraw умирает с пулом игрока — destroy не нужен, просто
    // сбрасываем состояние слота (id будет переиспользован новым игроком).
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    m_huds[playerId] = {};
}

IPlayerTextDraw *SpeedometerSystem::ensureShown(IPlayer &player, int &id, const Vector2 &position,
                                                const TextDrawParams &params)
{
    // Пул per-player текстдравов конечен — create вернёт nullptr при исчерпании,
    // тогда элемент просто не появится (id остаётся -1).
    if (id < 0)
    {
        IPlayerTextDraw *created = m_textDrawService.createForPlayer(player, position, "0", params);
        if (!created)
        {
            return nullptr;
        }
        id = created->getID();
    }

    // createForPlayer не делает авто-show — показываем явно.
    IPlayerTextDraw *td = m_textDrawService.getForPlayer(player, id);
    if (td)
    {
        td->show();
    }
    return td;
}

void SpeedometerSystem::showFor(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    Hud &hud = m_huds[playerId];

    ensureShown(player, hud.speedId, SPEED_POS, speedParams());
    ensureShown(player, hud.hpId, HP_POS, lineParams());
    ensureShown(player, hud.fuelId, FUEL_POS, lineParams());

    // Если даже основной элемент не создался (пул исчерпан) — HUD не активируем.
    if (hud.speedId < 0)
    {
        return;
    }
    hud.driving = true;

    // Кеш прошлой поездки сбрасываем к сентинелу: после hide/show текст надо
    // перерисовать, даже если значения совпали с последними отправленными.
    hud.lastSpeed = Hud::UNSENT;
    hud.lastHp = Hud::UNSENT;
    hud.lastFuel = Hud::UNSENT;

    // Сразу показать актуальные значения, не дожидаясь ближайшего тика.
    updateHud(player);
}

void SpeedometerSystem::hideFor(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    Hud &hud = m_huds[playerId];

    // Для hide() нужен живой IPlayer; если игрока уже нет — слот ещё чистится в
    // onPlayerDisconnect, здесь просто гасим флаг.
    if (IPlayer *player = m_core.getPlayers().get(playerId))
    {
        for (const int id : {hud.speedId, hud.hpId, hud.fuelId})
        {
            if (id < 0)
            {
                continue;
            }
            if (IPlayerTextDraw *td = m_textDrawService.getForPlayer(*player, id))
            {
                td->hide();
            }
        }
    }
    hud.driving = false;
}

void SpeedometerSystem::tick()
{
    for (int playerId = 0; playerId < MAX_PLAYERS; ++playerId)
    {
        Hud &hud = m_huds[playerId];
        if (!hud.driving)
        {
            continue;
        }

        IPlayer *player = m_core.getPlayers().get(playerId);
        if (!player)
        {
            // Игрок ушёл без события смены стейта — гасим флаг (слот уже сброшен
            // дисконнектом, либо сбросится позже).
            hud.driving = false;
            continue;
        }

        // Доп. сверка с источником правды: HUD держим только пока сервер видит
        // игрока на месте водителя (getSeat==0). Иначе скрываем.
        if (m_vehicleService.getSeat(playerId) != 0)
        {
            hideFor(playerId);
            continue;
        }

        updateHud(*player);
    }
}

void SpeedometerSystem::updateHud(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    Hud &hud = m_huds[playerId];

    // Машина водителя — нужна и для скорости, и для HP/топлива/приглушения.
    IVehicle *vehicle = m_vehicleService.getVehicle(playerId);
    if (!vehicle)
    {
        // Машины нет (уничтожили, а смена стейта ещё не дошла) — прячем прибор
        // целиком: «живой» ноль скорости рядом со стейл-цифрами HP/топлива
        // прежней машины врал бы. Вернётся на следующем входе за руль (showFor).
        hideFor(playerId);
        return;
    }
    const int vehicleId = vehicle->getID();

    // Скорость — из КЛИЕНТСКОЙ велосити машины (SA-юниты) -> км/ч, арифметическое
    // округление до целого (усечение int-кастом систематически занижало бы на
    // 1 км/ч). |v| = sqrt(x²+y²+z²). На остановке — «0».
    if (hud.speedId >= 0)
    {
        // Велосити приходит из клиентского sync без серверной валидации — читер
        // может прислать NaN/Inf. lround от не-финитного значения реализационно-
        // зависим, поэтому не-финитную скорость показываем как «0» (косметика, но
        // без мусора на приборе). Конечную клампим: long на MSVC x86 32-битный,
        // lround огромного float дал бы LONG_MIN == Hud::UNSENT и сломал кеш.
        const Vector3 v = m_vehicleService.getVelocity(vehicleId);
        const float raw = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) * SA_VELOCITY_TO_KMH;
        const long speed = std::isfinite(raw) ? std::lround(std::min(raw, 9999.0f)) : 0;
        // Текст шлём только на смене округлённого значения, не каждый тик.
        if (speed != hud.lastSpeed)
        {
            hud.lastSpeed = speed;
            m_textDrawService.setTextForPlayer(player, hud.speedId, fmt::format("{}", speed));
        }
    }

    // Двигатель заглушён (engine == 0; -1/1 — работает) -> число скорости тускнеет
    // до alpha 0x50. Цвет меняем только на смене состояния, не каждый тик.
    if (hud.speedId >= 0)
    {
        const bool engineOff = vehicle->getParams().engine == 0;
        if (engineOff != hud.speedDimmed)
        {
            hud.speedDimmed = engineOff;
            if (IPlayerTextDraw *td = m_textDrawService.getForPlayer(player, hud.speedId))
            {
                td->setColour(engineOff ? SPEED_COLOUR_OFF : SPEED_COLOUR_ON);
                td->restream(); // смена свойства textdraw применяется через перестрим
            }
        }
    }

    if (hud.hpId >= 0)
    {
        const long hp = std::lround(m_vehicleService.getHealth(vehicleId));
        if (hp != hud.lastHp)
        {
            hud.lastHp = hp;
            const long maxHp = std::lround(VehicleService::MAX_HEALTH);
            m_textDrawService.setTextForPlayer(player, hud.hpId, fmt::format("{}/{}", hp, maxHp));
        }
    }
    if (hud.fuelId >= 0)
    {
        const long fuel = std::lround(m_vehicleService.getFuel(vehicleId));
        if (fuel != hud.lastFuel)
        {
            hud.lastFuel = fuel;
            const long capacity = std::lround(VehicleService::FUEL_CAPACITY);
            m_textDrawService.setTextForPlayer(player, hud.fuelId, fmt::format("{}/{}", fuel, capacity));
        }
    }
}
