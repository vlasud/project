#pragma once

#include "Macro.h"
#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>

class ScreenTimerSystem;
struct IPlayer;

// Инфраструктурный (Core) сервис экранного таймера обратного отсчёта: ОДИН
// per-player textdraw-бар (верх-центр экрана), показывающий числовой остаток
// времени активного окна. Это ДИСПЛЕЙ-СЛОЙ — сам ничего не отсчитывает и не
// запускает игровых таймеров: бизнес-система (источник правды о времени) пушит
// остаток раз в секунду из своего тика. Один слот на игрока: новый show()
// заменяет предыдущий, hide() гасит. Состояние сессионное, БД нет.
//
// Макет — фиксированный (см. .cpp), пресет /td-редактора геймдизайнера
// (Server/textdraws/timer.txt): центр-верх, шрифт Pricedown, белый текст.
// Текст языко-нейтрален (M:SS); опциональная метка — ТОЛЬКО English.
class ScreenTimerService final : public IService
{
    friend ScreenTimerSystem;

  public:
    // Показать/обновить бар: остаток remainingSeconds (и опц. англ. метку label).
    // Повторный вызов на уже показанном баре обновляет ТОЛЬКО строку (лёгкий
    // SetString RPC, без рестрима — бар не мигает). remainingSeconds <= 0 —
    // защитный авто-hide (бизнес запушил истёкший остаток).
    void show(IPlayer &player, int remainingSeconds, StringView label = {});
    // Скрыть бар немедленно. Идемпотентна.
    void hide(IPlayer &player);

  private:
    // Вызываются только ScreenTimerSystem.
    void initialize(TextDrawService *textDrawService);
    void resetPlayer(int playerId);

    struct Layout
    {
        Vector2 position;
        TextDrawParams params;
    };
    static const Layout &layout();

    struct State
    {
        int textDrawId = -1; // -1 — per-player textdraw ещё не создан (лениво)
        bool shown = false;  // бар сейчас на экране
    };

    TextDrawService *m_textDrawService = nullptr;
    std::array<State, MAX_PLAYERS> m_states;
};
