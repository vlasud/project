#pragma once

#include "Macro.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>

class WorldSystem;
struct ICore;

// Мировое состояние: игровое время и погода. Единственный источник правды —
// никто не зовёт player.setTime/setWeather напрямую.
//
//   m_world.setTime(20, 30);                  // 20:30 у всех (и у новых)
//   m_world.setTimeFlowing(true, 1s);         // время течёт: 1 игровая минута за секунду
//   m_world.setWeather(8);                    // шторм всем
//   m_world.setWeatherForPlayer(player, 0);   // персональная погода (интерьер)
//   m_world.clearWeatherForPlayer(player);    // вернуть глобальную
//   m_world.showClock(true);                  // часы на HUD
//
// Новоприбывшим всё применяется на спавне (и через InitGame для часа/погоды).
// Время и погода — поток сервер -> клиент, клиентского ввода нет, абьюзить
// нечего (локальный визуальный хак погоды виден только самому читеру).
class WorldService final : public IService
{
    friend WorldSystem;

  public:
    static constexpr int MAX_WEATHER_ID = 45; // дальше клиент не знает таких погод

    // --- время ---
    void setTime(int hour, int minute);
    void getTime(int &hour, int &minute) const;
    // Течение времени: одна игровая минута за realPerGameMinute (выкл по умолчанию).
    void setTimeFlowing(bool flowing, Milliseconds realPerGameMinute = Milliseconds(1000));
    bool isTimeFlowing() const;

    // --- погода ---
    void setWeather(int weatherId); // глобально (персональные оверрайды переживают смену)
    int getWeather() const;
    void setWeatherForPlayer(IPlayer &player, int weatherId);
    void clearWeatherForPlayer(IPlayer &player);

    // --- часы на HUD ---
    void showClock(bool visible);
    bool isClockVisible() const;

    // --- глобальные настройки мира (флаги InitGame) ---
    // Стант-бонусы: клиент начисляет за трюки деньги МИМО PlayerMoneyService —
    // дыра в источнике правды о деньгах; WorldSystem выключает их при старте.
    // Применяется и к подключённым, и к новым.
    void setStuntBonuses(bool enable);
    bool stuntBonusesEnabled() const;

    // Стандартные входы в интерьеры GTA SA (жёлтые enex-маркеры): выключены —
    // все здания открываются только нашими пикапами (базы фракций и т.п.).
    // Клиент читает флаг ОДИН РАЗ в InitGame — задаётся при старте сервера.
    void setInteriorEnterExits(bool enable);
    bool interiorEnterExitsEnabled() const;

    // Нейм-теги и маркеры игроков на радаре клиент читает ОДИН РАЗ в InitGame:
    // смена на лету подействует только на новые подключения — задавайте при
    // старте сервера.
    void setNameTags(bool show);
    bool nameTagsEnabled() const;
    void setPlayerMarkerMode(PlayerMarkerMode mode);
    PlayerMarkerMode playerMarkerMode() const;

  private:
    // Вызываются WorldSystem.
    void initialize(ICore *core, TimerService *timers);
    void handleSpawn(IPlayer &player); // синхронизация состояния заспавнившемуся
    void resetPlayer(int playerId);

    void advanceMinute(); // тик течения времени
    void rescheduleClock();
    void broadcastTime();

    ICore *m_core = nullptr;
    TimerService *m_timers = nullptr;

    int m_hour = 12;
    int m_minute = 0;
    bool m_flowing = false;
    Milliseconds m_perGameMinute{1000};
    TimerService::Handle m_clockTimer{};

    int m_weather = 1;
    bool m_clockVisible = false;

    std::array<bool, MAX_PLAYERS> m_weatherOverride{};
    std::array<int, MAX_PLAYERS> m_overrideWeather{};
};
