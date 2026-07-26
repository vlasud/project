// Дев-бенчмарк собирается только под GAMEMODE_BENCHMARK (см. CMakeLists):
// в боевой сборке этих зондов и команды быть не должно.
#ifdef GAMEMODE_BENCHMARK

#include "Systems/Core/BenchmarkSystem/LoadTest.h"

#include <algorithm>

namespace
{
// Ожидаемое число синков за прогон: резервируем заранее, чтобы перевыделение
// вектора не попало внутрь измеряемого интервала.
constexpr std::size_t EXPECTED_SAMPLES = 60000;

double percentile(std::vector<double> &sorted, double ratio)
{
    if (sorted.empty())
        return 0.0;
    std::size_t index = static_cast<std::size_t>(ratio * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

double toMicros(TimePoint from, TimePoint to)
{
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count()) / 1000.0;
}
} // namespace

void LoadTest::begin(int bots, int humans)
{
    m_sample = Sample{};
    m_sample.bots = bots;
    m_sample.humans = humans;
    m_chainSamples.clear();
    m_chainSamples.reserve(EXPECTED_SAMPLES);
    m_sampleTick = 0;
    m_sampleStride = 1;
    m_chainOpen = false;
    m_running = true;
}

LoadTest::Sample LoadTest::end()
{
    m_running = false;
    m_chainOpen = false;

    // Сортировка уже вне замера: считаем типичный синк (медиана) и хвост (p95).
    std::sort(m_chainSamples.begin(), m_chainSamples.end());
    m_sample.chainMedianUs = percentile(m_chainSamples, 0.50);
    m_sample.chainP95Us = percentile(m_chainSamples, 0.95);
    return m_sample;
}

bool LoadTest::running() const
{
    return m_running;
}

void LoadTest::markChainStart(TimePoint now)
{
    if (!m_running)
        return;
    m_chainStart = now;
    m_chainOpen = true;
}

void LoadTest::markChainEnd(TimePoint now)
{
    // Без открытой отметки замер бессмыслен: обработчик-начало мог не сработать,
    // если синк был отброшен раньше по цепочке.
    if (!m_running || !m_chainOpen)
        return;
    m_chainOpen = false;

    const double us = toMicros(m_chainStart, now);
    ++m_sample.updates;
    m_sample.chainTotalUs += us;
    if (us > m_sample.chainMaxUs)
        m_sample.chainMaxUs = us;
    // Выборка для медианы и процентиля. Когда буфер заполняется, он НЕ обрезается
    // (иначе статистика считалась бы только по началу прогона — а начало дороже:
    // там стример досылает объекты): вместо этого выборка прореживается вдвое, и
    // дальше берётся каждый второй, четвёртый и так далее. Так распределение
    // остаётся равномерным по всему прогону при фиксированной памяти.
    ++m_sampleTick;
    if (m_sampleTick % m_sampleStride != 0)
        return;
    if (m_chainSamples.size() == m_chainSamples.capacity())
    {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < m_chainSamples.size(); i += 2)
            m_chainSamples[kept++] = m_chainSamples[i];
        m_chainSamples.resize(kept);
        m_sampleStride *= 2;
    }
    m_chainSamples.push_back(us);
}

void LoadTest::markTick(Microseconds elapsed)
{
    if (!m_running)
        return;
    const double us = static_cast<double>(elapsed.count());
    ++m_sample.ticks;
    m_sample.tickTotalUs += us;
    if (us > m_sample.tickMaxUs)
        m_sample.tickMaxUs = us;
}

#endif // GAMEMODE_BENCHMARK
