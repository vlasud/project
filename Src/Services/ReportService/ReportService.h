#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>
#include <chrono>
#include <string>

// Источник правды кулдауна репортов игрока (/mn -> «Связь с администрацией») и
// журнал обращений. Кулдаун нужен отдельно от корневого антифлуда команд: тот
// ловит лишь байт-в-байт одинаковые строки подряд, а репорт открывается из
// диалога и шлёт РАЗНЫЕ тексты — без локального барьера один тролль зальёт всем
// админам чат потоком разных жалоб.
//
// Сервис НЕ рассылает обращение сам — у сервисов нет доступа к игрокам/ICore.
// Рассылку залогиненным админам делает MenuSystem; сюда обращение приходит уже
// после санитизации (utf-8). Здесь — только кулдаун (память, сессия сервера) и
// запись в файл-лог.
//
// Пер-игрок: момент последнего успешного обращения. Сброс на дисконнекте
// обязателен — иначе кулдаун «протёк» бы в переиспользованный слот к следующему
// игроку (и баг, и абьюз: новый игрок без репортов получил бы паузу).
class ReportService final : public IService
{
  public:
    static constexpr std::chrono::seconds REPORT_COOLDOWN{60};

    // Кулдаун истёк (или это первый репорт). bounds-guard по id.
    bool ready(int playerId, TimePoint now) const;
    // Остаток кулдауна в секундах (ceil, как в антифлуде PlayerCommandService).
    // 0 — кулдаун истёк / первый репорт / id вне диапазона.
    int secondsLeft(int playerId, TimePoint now) const;

    // Зафиксировать УСПЕШНУЮ отправку: ставит кулдаун (lastReportAt = now) и
    // пишет обращение в файл-лог. text — уже utf-8 после санитизации.
    void record(int playerId, TimePoint now, const std::string &playerName, const std::string &text);

    // Сброс слота (зовётся на дисконнекте).
    void reset(int playerId);

  private:
    struct State
    {
        TimePoint lastReportAt{}; // epoch — репортов в этой сессии ещё не было
        bool used = false;        // был ли хоть один репорт (читается только при used == true)
    };

    std::array<State, MAX_PLAYERS> m_state;
};
