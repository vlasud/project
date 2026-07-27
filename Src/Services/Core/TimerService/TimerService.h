#pragma once

#include "Services/IService.h"
#include "Server/Components/Timers/timers.hpp"
#include "types.hpp"
#include <functional>
#include <unordered_map>

class TimerSystem;
struct IPlayer;
struct ICore;

// Планировщик — удобная обёртка над ITimersComponent для отложенных и
// периодических задач. Принимает std::chrono-длительности (секунды/минуты
// конвертируются неявно):
//
//   m_timers.setTimeout(5s, [] { ... });                 // один раз через 5 секунд
//   m_timers.setInterval(1min, [this] { paycheck(); });  // каждую минуту
//
//   // Пер-плеерный таймер: колбэк получает гарантированно живого игрока,
//   // при дисконнекте таймер отменяется автоматически.
//   auto handle = m_timers.setPlayerInterval(player, 1s, [](IPlayer &p) { ... });
//   m_timers.cancel(handle);
//
// Хэндл безопасен всегда: отмена сработавшего/отменённого таймера — no-op,
// отменять можно из любого места, в том числе из колбэка самого таймера.
class TimerService final : public IService
{
    friend TimerSystem;

  public:
    // Компонент таймеров может пережить геймод: порядок выгрузки компонентов ядром
    // не определён. Деструктор гасит наши таймеры и закрывает обратный путь, иначе
    // free() чужого таймера позвал бы handleFree на уже разрушенной мапе.
    ~TimerService();

    struct Handle
    {
        int id = 0; // 0 — пустой хэндл

        explicit operator bool() const
        {
            return id != 0;
        }
    };

    using Callback = std::function<void()>;
    using PlayerCallback = std::function<void(IPlayer &)>;

    // Один раз через delay.
    Handle setTimeout(Milliseconds delay, Callback callback);
    // Периодически, первый вызов через interval, до отмены.
    Handle setInterval(Milliseconds interval, Callback callback);
    // Общий случай: первый вызов через initial, далее каждые interval,
    // всего count вызовов (0 — без ограничения).
    Handle setRepeating(Milliseconds initial, Milliseconds interval, unsigned int count, Callback callback);

    // Пер-плеерные версии: авто-отмена при выходе игрока, колбэк со ссылкой
    // на живого игрока (гонка «таймер сработал в тик дисконнекта» исключена).
    Handle setPlayerTimeout(IPlayer &player, Milliseconds delay, PlayerCallback callback);
    Handle setPlayerInterval(IPlayer &player, Milliseconds interval, PlayerCallback callback);

    // Отменить и обнулить хэндл. Идемпотентна.
    void cancel(Handle &handle);

    bool isActive(Handle handle) const;
    // Время до ближайшего срабатывания; 0, если таймер не активен.
    Milliseconds remaining(Handle handle) const;

  private:
    class HandlerImpl; // мост TimerTimeOutHandler -> handleTimeout/handleFree

    struct Entry
    {
        ITimer *timer = nullptr;
        HandlerImpl *handler = nullptr; // владеет компонент; нужен для detach в деструкторе
        int playerId = -1;              // -1 — таймер не привязан к игроку
        Callback callback;
        PlayerCallback playerCallback;
    };

    // Вызываются TimerSystem.
    void initialize(ICore *core, ITimersComponent *timers);
    void resetPlayer(int playerId);

    Handle createTimer(Milliseconds initial, Milliseconds interval, unsigned int count, int playerId,
                       Callback callback, PlayerCallback playerCallback);
    void handleTimeout(int id);
    void handleFree(int id);

    ICore *m_core = nullptr;
    ITimersComponent *m_timers = nullptr;
    // Записи удаляются ТОЛЬКО в handleFree (когда компонент освобождает таймер):
    // cancel() лишь делает kill. Поэтому колбэк может отменять свой же таймер —
    // его std::function не разрушается во время собственного вызова.
    std::unordered_map<int, Entry> m_entries;
    int m_nextId = 0;
    bool m_shuttingDown = false; // в разрушении: handleTimeout/handleFree ничего не трогают
};
