#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <Server/Components/TextLabels/textlabels.hpp>
#include <array>
#include <functional>
#include <string>
#include <vector>

class PlayerActivitySystem;
struct ICore;

// Детект паузы клиента (ESC): на паузе клиент перестаёт слать синк, и пропажа
// апдейтов дольше порога означает паузу. Над головой паузнутого игрока висит
// лейбл «На паузе N секунд/минут» (растёт раз в секунду, потолок «>30 минут»),
// остальные игроки видят, что человек отошёл.
//
//   m_activity.isPaused(playerId);
//   m_activity.pausedFor(playerId, now);            // длительность паузы
//   m_activity.onPaused([](IPlayer &p) { ... });    // события для анти-АФК и пр.
//   m_activity.onResumed([](IPlayer &p) { ... });
//
// Честность: пауза выводится из ОТСУТСТВИЯ синка — «сымитировать активность»
// можно только реально слая синк (то есть не быть на паузе), а «сымитировать
// паузу» — только перестав слать (то есть реально потеряв управление). Лейбл
// генерируется сервером — клиент на него не влияет. Wasted-экран смерти и
// спектейт паузой не считаются.
class PlayerActivityService final : public IService
{
    friend PlayerActivitySystem;

  public:
    using Handler = std::function<void(IPlayer &)>;

    bool isPaused(int playerId) const;
    // Сколько игрок на паузе (0 — активен).
    Milliseconds pausedFor(int playerId, TimePoint now) const;

    // Подписки на события (вызываются из конструкторов систем).
    void onPaused(Handler handler);
    void onResumed(Handler handler);

  private:
    struct Slot
    {
        bool tracked = false; // получен хотя бы один апдейт за это подключение
        bool paused = false;
        TimePoint lastUpdate{};
        int labelId = -1;     // лейбл «На паузе ...» в глобальном пуле
        std::string lastText; // последний установленный текст (cp1251) — без лишних пакетов
    };

    // Вызываются PlayerActivitySystem.
    void initialize(ICore *core, ITextLabelsComponent *labels);
    void handleConnect(IPlayer &player, TimePoint now);
    void handleUpdate(IPlayer &player, TimePoint now);
    void sweep(TimePoint now); // раз в секунду из таймера: детект пауз и обновление лейблов
    void resetPlayer(int playerId);

    void markPaused(IPlayer &player, TimePoint now);
    void markResumed(IPlayer &player);
    void updateLabel(IPlayer &player, Slot &slot, Milliseconds duration);
    void hideLabel(Slot &slot);
    static std::string pauseText(Milliseconds duration); // utf-8

    ICore *m_core = nullptr;
    ITextLabelsComponent *m_labels = nullptr;

    std::array<Slot, MAX_PLAYERS> m_slots;
    std::vector<Handler> m_pausedHandlers;
    std::vector<Handler> m_resumedHandlers;
};
