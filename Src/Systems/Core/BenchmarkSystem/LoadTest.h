#pragma once

// Дев-бенчмарк собирается только под GAMEMODE_BENCHMARK (см. CMakeLists):
// в боевой сборке этих зондов и команды быть не должно.
#ifdef GAMEMODE_BENCHMARK

#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <Server/Components/NPCs/npcs.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// Нагрузочный замер на ботах: сколько РЕАЛЬНО стоит обработка синков, когда на
// сервере есть онлайн. В отличие от микробенчмарка меряется не отдельная функция,
// а вся цепочка обработчиков onPlayerUpdate целиком — то есть настоящая цена
// одного синка со всеми системами, стримером и анти-читом.
//
// КАК МЕРЯЕТСЯ ЦЕПОЧКА. Два пустых обработчика подписаны на один диспатчер с
// крайними приоритетами: первый (Highest) ставит отметку времени, последний
// (Lowest) считает разницу. Между ними ядро успевает прогнать все остальные
// системы, поэтому разница и есть стоимость синка для геймода.
//
// ЧТО БОТЫ ДАЮТ И ЧЕГО НЕ ДАЮТ:
//  * дают — поток foot-синков, работу стримера и грида, валидацию анти-чита,
//    масштабирование от числа игроков;
//  * НЕ дают — сетевой слой (NPC живут внутри процесса), урон между игроками
//    (его шлёт настоящий клиент), диалоги, команды и нагрузку на БД.
// Поэтому цифры отсюда — нижняя граница: в бою тот же онлайн дороже.
class LoadTest
{
  public:
    struct Sample
    {
        std::uint64_t updates = 0; // синков за прогон
        double chainTotalUs = 0.0; // суммарное время цепочки
        double chainMaxUs = 0.0;   // худший синк
        // Медиана и 95-й процентиль: по среднему и максимуму судить нельзя —
        // единичная досылка объектов стримером даёт выброс в тысячу раз выше
        // типичного синка и утягивает обе цифры.
        double chainMedianUs = 0.0;
        double chainP95Us = 0.0;
        std::uint64_t ticks = 0;   // тиков сервера за прогон
        double tickTotalUs = 0.0;  // суммарное время между тиками
        double tickMaxUs = 0.0;    // худший разрыв между тиками
        int bots = 0;              // сколько ботов стояло на карте
        int humans = 0;            // сколько живых игроков было онлайн
        std::chrono::seconds duration{0};
    };

    // Замер цепочки: вызывается обработчиками с крайними приоритетами.
    void markChainStart(TimePoint now);
    void markChainEnd(TimePoint now);
    void markTick(Microseconds elapsed);

    void begin(int bots, int humans);
    Sample end();
    bool running() const;

  private:
    bool m_running = false;
    TimePoint m_chainStart;
    bool m_chainOpen = false;
    Sample m_sample;
    // Выборка синков для медианы и процентиля. Память резервируется заранее, чтобы
    // рост вектора не попал в замер; при переполнении выборка прореживается (см.
    // markChainEnd), поэтому она покрывает весь прогон, а не только его начало.
    std::vector<double> m_chainSamples;
    std::uint64_t m_sampleTick = 0;
    std::uint64_t m_sampleStride = 1;
};

#endif // GAMEMODE_BENCHMARK
