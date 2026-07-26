#pragma once

// Дев-бенчмарк собирается только под GAMEMODE_BENCHMARK (см. CMakeLists):
// в боевой сборке этих зондов и команды быть не должно.
#ifdef GAMEMODE_BENCHMARK

#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Systems/BaseSystem.h"
#include "Systems/Core/BenchmarkSystem/LoadTest.h"
#include "player.hpp"
#include <Server/Components/NPCs/npcs.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Синтетический бенчмарк горячих операций (/bench, только разработчик).
//
// Меряет НАШУ логику, а не сервер целиком: сколько наносекунд стоит один вызов
// каждой операции, которая крутится в per-update пути или в обработке событий.
// Результат — таблица, отсортированная по убыванию стоимости: сверху то, что
// первым упрётся в потолок при росте онлайна.
//
// ОГРАНИЧЕНИЯ, о которых нужно помнить, читая цифры:
//  * это микробенчмарк: кэш прогрет, ветвления предсказаны, конкуренции нет —
//    в бою те же операции обычно дороже;
//  * меряется стоимость ОДНОГО вызова, а не нагрузка сервера: умножать на число
//    игроков и частоту синка нужно самому;
//  * прогон синхронный и на несколько сотен миллисекунд подвешивает сервер,
//    поэтому команда закрыта уровнем разработчика.
//
// Кейсы подобраны так, чтобы НЕ менять состояние: только чтение и чистые функции.
// Ничего, что пишет игроку, шлёт RPC или фиксирует нарушения, здесь нет.
class BenchmarkSystem : public BaseSystem, public CoreEventHandler, public NPCEventHandler
{
  public:
    BenchmarkSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    // Время между тиками сервера — верхняя рамка для всего остального.
    void onTick(Microseconds elapsed, TimePoint now) override;
    // Бот дошёл до точки — отправляем к следующей, иначе поток синков затухнет.
    void onNPCFinishMove(INPC &npc) override;

  private:
    // Замер цепочки: два обработчика с крайними приоритетами. Между ними ядро
    // прогоняет все системы, поэтому разница их отметок — цена синка для геймода.
    struct ChainProbe : public PlayerUpdateEventHandler
    {
        LoadTest *test = nullptr;
        bool start = false;
        bool onPlayerUpdate(IPlayer &, TimePoint) override
        {
            // Зонды висят на диспатчере всегда, поэтому вне прогона выходим ДО
            // взятия отметки времени: steady_clock::now() стоит десятки наносекунд,
            // и умножать их на онлайн × частоту синков ради выключенного замера
            // незачем.
            if (!test || !test->running())
            {
                return true;
            }
            // Время берём СВОЁ, а не параметр обработчика: ядро вычисляет его один
            // раз до диспатча и передаёт всем одинаковым, так что разница между
            // началом и концом цепочки по нему всегда ноль.
            const TimePoint now = std::chrono::steady_clock::now();
            start ? test->markChainStart(now) : test->markChainEnd(now);
            return true;
        }
    };

    struct Result
    {
        std::string name;
        double nsPerCall = 0.0;
        std::uint64_t calls = 0;
    };

    // Кейс: имя и тело, вызываемое iterations раз. Возвращает контрольную сумму —
    // она нужна, чтобы оптимизатор не выбросил измеряемый код целиком.
    struct Case
    {
        std::string name;
        std::function<std::uint64_t(IPlayer &player, std::uint64_t iterations)> run;
    };

    void buildCases();
    void runAll(IPlayer &player);
    void showResults(IPlayer &player);

    // --- нагрузочный прогон на ботах ---
    void showMenu(IPlayer &player);
    void startLoad(IPlayer &player, int bots, float spreadRadius);
    // Спавн порциями: тысяча NPC разом занимает секунды, всё это время сервер не
    // отвечает и выбрасывает живых игроков по таймауту.
    void spawnBatch();
    void beginMeasurement();
    void finishLoad();
    void showLoadResult(IPlayer &player, const LoadTest::Sample &sample);
    void sendBotSomewhere(INPC &npc);
    void removeBots();
    int humansOnline() const;

    std::vector<Case> m_cases;
    std::vector<Result> m_results; // последний прогон, отсортирован по убыванию
    std::string m_lastRunBy;       // кто и когда гонял — чтобы не путать чужие цифры
    double m_baselineNs = 0.0;     // накладные расходы самого измерения

    PlayerDialogService &m_dialogService;
    TimerService &m_timerService;
    INPCComponent *m_npcs = nullptr;

    LoadTest m_load;
    ChainProbe m_chainStart; // подписан с EventPriority_Highest
    ChainProbe m_chainEnd;   // подписан с EventPriority_Lowest
    // ID, а не указатели: бота может убрать кто угодно (кик, ошибка, чужая
    // система), и сырой INPC* тихо становится висячим — на нём сервер и падал.
    // Через пул мы каждый раз проверяем, жив ли он ещё.
    std::vector<int> m_botIds;
    Vector3 m_botCenter{};
    float m_botRadius = 50.0f;
    int m_loadStarter = -1; // кому показать отчёт по окончании
    // Перенацеливание ботов по таймеру: дошедший до точки NPC замирает и почти
    // перестаёт слать синки (в конфиге стоит пропуск неизменных апдейтов), а
    // замер стоячего онлайна не имеет смысла.
    TimerService::Handle m_botRetarget;
    TimerService::Handle m_botSpawner;
    int m_botsRequested = 0; // сколько ещё осталось создать
    int m_botsPlanned = 0;   // сколько просили изначально — для отчёта о нехватке слотов
};

#endif // GAMEMODE_BENCHMARK
