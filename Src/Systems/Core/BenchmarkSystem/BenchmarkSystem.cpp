// Дев-бенчмарк собирается только под GAMEMODE_BENCHMARK (см. CMakeLists):
// в боевой сборке этих зондов и команды быть не должно.
#ifdef GAMEMODE_BENCHMARK

#include "Systems/Core/BenchmarkSystem/BenchmarkSystem.h"

#include "Macro.h"

#include "Services/AdminService/AdminService.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/GridService/GridService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerVelocityService/PlayerVelocityService.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
#include "Services/Core/StreamerService/StreamerService.h"
#include "Systems/Core/PlayerHealthSystem/WeaponLimits.h"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <glm/geometric.hpp>

namespace
{
const Colour DEBUG_COLOUR{120, 220, 255};

// Итераций на кейс. Достаточно, чтобы усреднить шум таймера (его разрешение
// ~100 нс), и мало, чтобы весь прогон уложился в доли секунды.
constexpr std::uint64_t ITERATIONS = 20000;
// Тяжёлым кейсам (обход сетки) столько не нужно — они и так дороже на порядки.
constexpr std::uint64_t HEAVY_ITERATIONS = 2000;

// Нагрузочный прогон: сначала сцена устаканивается (боты расходятся, стример
// досылает объекты), потом идёт замер — иначе всплеск создания попадёт в среднее.
// Как часто боту выдаётся новая цель, чтобы он не замирал между точками.
constexpr Seconds BOT_RETARGET{3};
// Спавн ботов порциями: столько за один шаг таймера и с таким шагом.
constexpr int SPAWN_BATCH = 25;
constexpr Milliseconds SPAWN_STEP{150};
// Слоты, которые не отдаём ботам ни при каких сценариях — под живых игроков.
constexpr int SLOT_RESERVE = 10;
constexpr Seconds LOAD_WARMUP{5};
constexpr Seconds LOAD_DURATION{20};

// Типичные строки для кейсов кодировки: примерно то, что реально ходит в чат.
const std::string SAMPLE_UTF8 = "Игрок Lo_Vlasud получил 1500 рублей за смену в порту";
const std::string SAMPLE_DIRTY = "{FF0000}Ник~r~ с цветокодами {00FF00}и мусором";

double measureNs(const std::function<std::uint64_t(std::uint64_t)> &body, std::uint64_t iterations, std::uint64_t &sink)
{
    const auto start = std::chrono::steady_clock::now();
    sink += body(iterations);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    return iterations ? ns / static_cast<double>(iterations) : 0.0;
}
} // namespace

BenchmarkSystem::BenchmarkSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_timerService(serviceRegister.getService<TimerService>())
{
    buildCases();

    // Крайние приоритеты: между этими двумя обработчиками ядро прогоняет все
    // остальные системы — ровно то, что мы и хотим измерить.
    m_chainStart.test = &m_load;
    m_chainStart.start = true;
    m_chainEnd.test = &m_load;
    m_chainEnd.start = false;
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(&m_chainStart, EventPriority_Highest);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(&m_chainEnd, EventPriority_Lowest);
    core.getEventDispatcher().addEventHandler(this);

    serviceRegister.getService<PlayerCommandService>().add(
        "bench", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            showMenu(player);
        },
        PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "бенчмарк: микрозамеры и нагрузка ботами",
        PlayerCommandService::HelpCategory::Hidden);
}

void BenchmarkSystem::initialize(IComponentList *components)
{
    // Компонент NPC опционален: без NPCs.dll нагрузочный прогон просто недоступен.
    m_npcs = components->queryComponent<INPCComponent>();
    if (m_npcs)
    {
        m_npcs->getEventDispatcher().addEventHandler(this);
    }
}

void BenchmarkSystem::onTick(Microseconds elapsed, TimePoint now)
{
    m_load.markTick(elapsed);
}

void BenchmarkSystem::buildCases()
{
    auto &location = m_serviceRegister.getService<PlayerLocationService>();
    auto &velocity = m_serviceRegister.getService<PlayerVelocityService>();
    auto &grid = m_serviceRegister.getService<GridService>();
    auto &antiCheat = m_serviceRegister.getService<AntiCheatService>();

    // Обход сетки в радиусе стрима — самый горячий запрос в игре: он идёт на
    // каждый стрим-тик каждого игрока и определяет, как сервер держит онлайн.
    m_cases.push_back(
        {"grid: обход радиуса стрима (300 м)", [&grid, &location](IPlayer &player, std::uint64_t iterations)
         {
             const Vector3 position = location.getPosition(player.getID());
             std::uint64_t seen = 0;
             for (std::uint64_t i = 0; i < iterations; ++i)
             {
                 grid.forEachInRadius(position, StreamerService::MAX_STREAM_DISTANCE,
                                      gridMask(GridEntityType::Object) | gridMask(GridEntityType::Pickup) |
                                          gridMask(GridEntityType::MapIcon) | gridMask(GridEntityType::TextLabel),
                                      [&seen](GridEntityType, std::int32_t id, float)
                                      {
                                          seen += id;
                                      });
             }
             return seen;
         }});

    // Тот же обход, но радиусом ближнего боя: показывает, насколько цена зависит
    // от радиуса, а не от числа сущностей вообще.
    m_cases.push_back({"grid: обход радиуса 30 м", [&grid, &location](IPlayer &player, std::uint64_t iterations)
                       {
                           const Vector3 position = location.getPosition(player.getID());
                           std::uint64_t seen = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                           {
                               grid.forEachInRadius(position, 30.0f,
                                                    gridMask(GridEntityType::Player) |
                                                        gridMask(GridEntityType::Vehicle),
                                                    [&seen](GridEntityType, std::int32_t id, float)
                                                    {
                                                        seen += id;
                                                    });
                           }
                           return seen;
                       }});

    m_cases.push_back({"location: принятая позиция", [&location](IPlayer &player, std::uint64_t iterations)
                       {
                           std::uint64_t sum = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                               sum += static_cast<std::uint64_t>(location.getPosition(player.getID()).x);
                           return sum;
                       }});

    m_cases.push_back({"velocity: скорость игрока", [&velocity](IPlayer &player, std::uint64_t iterations)
                       {
                           std::uint64_t sum = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                               sum += static_cast<std::uint64_t>(velocity.getSpeed(player.getID()));
                           return sum;
                       }});

    m_cases.push_back({"SDK: чтение позиции у ядра", [](IPlayer &player, std::uint64_t iterations)
                       {
                           std::uint64_t sum = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                               sum += static_cast<std::uint64_t>(player.getPosition().x);
                           return sum;
                       }});

    m_cases.push_back({"SDK: чтение данных прицела", [](IPlayer &player, std::uint64_t iterations)
                       {
                           std::uint64_t sum = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                               sum += static_cast<std::uint64_t>(player.getAimData().camFrontVector.x);
                           return sum;
                       }});

    m_cases.push_back(
        {"анти-чит: таблица темпа стрельбы", [](IPlayer &, std::uint64_t iterations)
         {
             std::uint64_t sum = 0;
             for (std::uint64_t i = 0; i < iterations; ++i)
                 sum += static_cast<std::uint64_t>(
                     PlayerWeaponService::minShotInterval(static_cast<std::uint8_t>(22 + i % 13)).count());
             return sum;
         }});

    m_cases.push_back({"анти-чит: вес нарушения", [&antiCheat](IPlayer &, std::uint64_t iterations)
                       {
                           std::uint64_t sum = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                               sum += static_cast<std::uint64_t>(
                                   antiCheat.weight(static_cast<AntiCheatService::ViolationType>(i % 18)) * 100.0f);
                           return sum;
                       }});

    m_cases.push_back({"урон: лимиты оружия", [](IPlayer &, std::uint64_t iterations)
                       {
                           std::uint64_t sum = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                           {
                               const WeaponLimits::Info *info = WeaponLimits::get(static_cast<unsigned>(i % 47));
                               sum += info ? static_cast<std::uint64_t>(info->maxDamage) : 0;
                           }
                           return sum;
                       }});

    m_cases.push_back({"строки: utf8 -> cp1251", [](IPlayer &, std::uint64_t iterations)
                       {
                           std::uint64_t sum = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                               sum += Encoding::utf8Tocp1251(SAMPLE_UTF8).size();
                           return sum;
                       }});

    m_cases.push_back({"строки: чистка цветокодов", [](IPlayer &, std::uint64_t iterations)
                       {
                           std::uint64_t sum = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                               sum += Encoding::neutralizeColorCodes(SAMPLE_DIRTY).size();
                           return sum;
                       }});

    m_cases.push_back({"строки: fmt::format", [](IPlayer &, std::uint64_t iterations)
                       {
                           std::uint64_t sum = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                               sum += fmt::format("player {} speed {:.1f} m/s", i, 7.5).size();
                           return sum;
                       }});

    m_cases.push_back({"математика: distance (эталон)", [&location](IPlayer &player, std::uint64_t iterations)
                       {
                           const Vector3 position = location.getPosition(player.getID());
                           std::uint64_t sum = 0;
                           for (std::uint64_t i = 0; i < iterations; ++i)
                               sum += static_cast<std::uint64_t>(
                                   glm::distance(position, Vector3(position.x + static_cast<float>(i), 0.0f, 0.0f)));
                           return sum;
                       }});
}

void BenchmarkSystem::runAll(IPlayer &player)
{
    m_results.clear();
    std::uint64_t sink = 0;

    // Базовая линия: стоимость самого цикла и вызова через std::function. Всё, что
    // ниже неё, измерению не поддаётся — так и надо читать такие строки.
    m_baselineNs = measureNs(
        [](std::uint64_t iterations)
        {
            std::uint64_t sum = 0;
            for (std::uint64_t i = 0; i < iterations; ++i)
                sum += i;
            return sum;
        },
        ITERATIONS, sink);

    for (const Case &benchCase : m_cases)
    {
        // Тяжёлым кейсам (обход сетки) хватает меньшего числа итераций.
        const std::uint64_t iterations = benchCase.name.rfind("grid:", 0) == 0 ? HEAVY_ITERATIONS : ITERATIONS;
        const double ns = measureNs(
            [&benchCase, &player](std::uint64_t count)
            {
                return benchCase.run(player, count);
            },
            iterations, sink);
        m_results.push_back({benchCase.name, ns, iterations});
    }

    // Сортировка по убыванию: сверху то, что дороже всего.
    std::sort(m_results.begin(), m_results.end(),
              [](const Result &a, const Result &b)
              {
                  return a.nsPerCall > b.nsPerCall;
              });

    m_lastRunBy = std::string(player.getName().data(), player.getName().size());

    // Контрольная сумма никуда не идёт, но без её использования оптимизатор вправе
    // выбросить весь измеряемый код.
    if (sink == 0xFFFFFFFFFFFFFFFFull)
    {
        player.sendClientMessage(DEBUG_COLOUR, "sink");
    }
}

void BenchmarkSystem::showMenu(IPlayer &player)
{
    // Радиус растёт вместе с числом ботов: тысяча NPC в радиусе 300 м — это не
    // «онлайн сервера», а искусственная давка, где стример уводит замер в свой
    // худший случай. Пропорция примерно как на живой карте.
    const std::string body = fmt::format("Микрозамеры горячих операций\t{} кейсов\n"
                                         "Нагрузка: 20 ботов рядом (толпа)\tрадиус 30 м\n"
                                         "Нагрузка: 20 ботов по району\tрадиус 300 м\n"
                                         "Нагрузка: 50 ботов\tрадиус 500 м\n"
                                         "Нагрузка: 300 ботов\tрадиус 1200 м\n"
                                         "Нагрузка: 500 ботов\tрадиус 1600 м\n"
                                         "Нагрузка: 1000 ботов\tрадиус 2500 м\n"
                                         "Убрать ботов\tсейчас: {}",
                                         m_cases.size(), m_botIds.size());

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST, "Бенчмарк", body, "Выбрать", "Закрыть"),
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *target = m_core.getPlayers().get(playerId);
                             if (!target || response != DialogResponse_Left)
                                 return;
                             switch (listItem)
                             {
                             case 0:
                                 runAll(*target);
                                 showResults(*target);
                                 return;
                             case 1:
                                 startLoad(*target, 20, 30.0f);
                                 return;
                             case 2:
                                 startLoad(*target, 20, 300.0f);
                                 return;
                             case 3:
                                 startLoad(*target, 50, 500.0f);
                                 return;
                             case 4:
                                 startLoad(*target, 300, 1200.0f);
                                 return;
                             case 5:
                                 startLoad(*target, 500, 1600.0f);
                                 return;
                             case 6:
                                 startLoad(*target, 1000, 2500.0f);
                                 return;
                             case 7:
                                 removeBots();
                                 target->sendClientMessage(DEBUG_COLOUR, u("Боты убраны"));
                                 return;
                             default:
                                 return;
                             }
                         });
}

int BenchmarkSystem::humansOnline() const
{
    int humans = 0;
    for (IPlayer *player : m_core.getPlayers().entries())
    {
        if (player && !player->isBot())
            ++humans;
    }
    return humans;
}

void BenchmarkSystem::sendBotSomewhere(INPC &npc)
{
    // Случайная точка в круге вокруг центра прогона: боты всё время в движении,
    // иначе поток синков вырождается и замер показывает простой.
    const float angle = static_cast<float>(std::rand() % 628) / 100.0f;
    const float distance = static_cast<float>(std::rand() % static_cast<int>(m_botRadius * 100.0f + 1)) / 100.0f;
    const Vector3 target{m_botCenter.x + std::cos(angle) * distance, m_botCenter.y + std::sin(angle) * distance,
                         m_botCenter.z};
    npc.move(target, NPCMoveType_Jog);
}

void BenchmarkSystem::onNPCFinishMove(INPC &npc)
{
    if (m_load.running())
    {
        sendBotSomewhere(npc);
    }
}

void BenchmarkSystem::startLoad(IPlayer &player, int bots, float spreadRadius)
{
    if (!m_npcs)
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Компонент NPC не загружен — нагрузочный прогон недоступен"));
        return;
    }
    if (m_load.running() || m_botsRequested > 0)
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Прогон уже идёт — дождитесь отчёта"));
        return;
    }

    removeBots();
    m_botCenter = player.getPosition();
    m_botRadius = spreadRadius;
    m_loadStarter = player.getID();

    // Потолок по слотам: пер-игроковые массивы сервисов рассчитаны на MAX_PLAYERS,
    // и живым игрокам нужно оставить место.
    const int slotCap = MAX_PLAYERS - humansOnline() - SLOT_RESERVE;
    m_botsPlanned = std::min(bots, std::max(slotCap, 0));
    m_botsRequested = m_botsPlanned;
    if (m_botsRequested <= 0)
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Нет свободных слотов под ботов"));
        return;
    }

    player.sendClientMessage(
        DEBUG_COLOUR, u(fmt::format("Создаю {} ботов порциями по {}, радиус {:.0f} м",
                                    m_botsRequested, SPAWN_BATCH, spreadRadius)));

    // Порциями по таймеру: разом тысяча NPC создаётся секундами, и всё это время
    // сервер не отвечает — живых игроков вышибает по таймауту.
    m_botSpawner = m_timerService.setInterval(SPAWN_STEP,
                                              [this]()
                                              {
                                                  spawnBatch();
                                              });
}

void BenchmarkSystem::spawnBatch()
{
    if (!m_npcs)
    {
        m_botsRequested = 0;
        return;
    }

    for (int i = 0; i < SPAWN_BATCH && m_botsRequested > 0; ++i)
    {
        INPC *npc = m_npcs->create(fmt::format("Bench_{}", m_botIds.size()));
        if (!npc)
        {
            // Слоты кончились раньше плана — работаем с тем, что создалось.
            m_botsRequested = 0;
            break;
        }
        --m_botsRequested;
        m_botIds.push_back(npc->getID());
        npc->spawn();
        npc->setPosition(m_botCenter, true);
        sendBotSomewhere(*npc);
    }

    if (m_botsRequested > 0)
    {
        return; // ещё не всех создали
    }

    m_timerService.cancel(m_botSpawner);
    beginMeasurement();
}

void BenchmarkSystem::beginMeasurement()
{
    IPlayer *player = m_core.getPlayers().get(m_loadStarter);
    if (m_botIds.empty())
    {
        if (player)
        {
            player->sendClientMessage(DEBUG_COLOUR,
                                      u("Не удалось создать ботов: проверьте max_players в config.json"));
        }
        return;
    }

    // Держим ботов в движении принудительно: одного события «дошёл до точки» мало —
    // NPC успевает замереть, а стоячий бот почти не шлёт синков (в конфиге стоит
    // пропуск неизменных апдейтов), и замер показывает простой вместо нагрузки.
    m_botRetarget = m_timerService.setInterval(BOT_RETARGET,
                                               [this]()
                                               {
                                                   if (!m_npcs)
                                                       return;
                                                   for (int id : m_botIds)
                                                   {
                                                       if (INPC *npc = m_npcs->get(id))
                                                           sendBotSomewhere(*npc);
                                                   }
                                               });

    if (player)
    {
        player->sendClientMessage(
            DEBUG_COLOUR, u(fmt::format("Создано {} из {} ботов; прогрев {} сек, замер {} сек",
                                        m_botIds.size(), m_botsPlanned, LOAD_WARMUP.count(), LOAD_DURATION.count())));
    }

    // Даём сцене устояться (боты расходятся, стример досылает объекты), и только
    // потом открываем счётчики — иначе в среднее попадёт всплеск создания.
    m_timerService.setTimeout(LOAD_WARMUP,
                              [this]()
                              {
                                  m_load.begin(static_cast<int>(m_botIds.size()), humansOnline());
                                  m_timerService.setTimeout(LOAD_DURATION,
                                                            [this]()
                                                            {
                                                                finishLoad();
                                                            });
                              });
}

void BenchmarkSystem::finishLoad()
{
    const LoadTest::Sample sample = m_load.end();
    m_timerService.cancel(m_botRetarget); // отмена не из своего колбэка — безопасно
    removeBots();

    IPlayer *player = m_core.getPlayers().get(m_loadStarter);
    m_loadStarter = -1;
    if (player)
    {
        showLoadResult(*player, sample);
    }
}

void BenchmarkSystem::removeBots()
{
    if (!m_npcs)
    {
        m_botIds.clear();
        return;
    }
    for (int id : m_botIds)
    {
        // Бот мог уйти сам (кик, ошибка сети) — тогда в пуле его уже нет, и
        // destroy() по сохранённому указателю был бы обращением к мёртвой памяти.
        if (INPC *npc = m_npcs->get(id))
        {
            m_npcs->destroy(*npc);
        }
    }
    m_botIds.clear();
}

void BenchmarkSystem::showLoadResult(IPlayer &player, const LoadTest::Sample &sample)
{
    // Плотность вокруг центра прогона: цена синка зависит от того, сколько
    // сущностей стример видит в радиусе. Без этих чисел два прогона в разных
    // местах карты выглядят как разница в коде, хотя это разница в декорациях.
    auto &grid = m_serviceRegister.getService<GridService>();
    int staticNear = 0;
    int playersNear = 0;
    grid.forEachInRadius(m_botCenter, StreamerService::MAX_STREAM_DISTANCE,
                         gridMask(GridEntityType::Object) | gridMask(GridEntityType::Pickup) |
                             gridMask(GridEntityType::MapIcon) | gridMask(GridEntityType::TextLabel),
                         [&staticNear](GridEntityType, std::int32_t, float) { ++staticNear; });
    grid.forEachInRadius(m_botCenter, StreamerService::MAX_STREAM_DISTANCE, gridMask(GridEntityType::Player),
                         [&playersNear](GridEntityType, std::int32_t, float) { ++playersNear; });

    if (sample.updates == 0)
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Прогон не собрал ни одного синка"));
        return;
    }

    const double seconds = static_cast<double>(LOAD_DURATION.count());
    const double avgChain = sample.chainTotalUs / static_cast<double>(sample.updates);
    const double updatesPerSec = static_cast<double>(sample.updates) / seconds;
    const double avgTick = sample.ticks ? sample.tickTotalUs / static_cast<double>(sample.ticks) : 0.0;
    // Доля процессорного времени, которую съела обработка синков за прогон.
    const double loadPercent = sample.chainTotalUs / (seconds * 1'000'000.0) * 100.0;
    const int players = sample.bots + sample.humans;
    // Грубая экстраполяция: цена синка меняется с онлайном (стример, грид), но
    // порядок «сколько ещё влезет» она показывает.
    const double perPlayerUs = players > 0 ? sample.chainTotalUs / seconds / players : 0.0;

    const std::string body = fmt::format("Онлайн в прогоне\t{} ({} ботов + {} живых)\n"
                                         "Синков обработано\t{} ({:.0f}/сек)\n"
                                         "Синков на игрока\t{:.1f}/сек\n"
                                         "Цепочка onPlayerUpdate: типичный\t{:.1f} мкс (медиана)\n"
                                         "Цепочка onPlayerUpdate: 95%\t{:.1f} мкс\n"
                                         "Цепочка onPlayerUpdate: среднее\t{:.1f} мкс\n"
                                         "Цепочка onPlayerUpdate: худший\t{:.1f} мкс\n"
                                         "Доля CPU на синки\t{:.2f}%\n"
                                         "Тик сервера: средний\t{:.0f} мкс\n"
                                         "Тик сервера: худший\t{:.0f} мкс\n"
                                         "Цена одного игрока\t{:.1f} мкс/сек\n"
                                         "Сущностей в сетке\t{}\n"
                                         "Активных пикапов\t{}\n"
                                         "Центр прогона\t{:.0f} {:.0f} {:.0f}\n"
                                         "В радиусе стрима: статика\t{}\n"
                                         "В радиусе стрима: игроки\t{}",
                                         players, sample.bots, sample.humans, sample.updates, updatesPerSec,
                                         players > 0 ? updatesPerSec / players : 0.0, sample.chainMedianUs,
                                         sample.chainP95Us, avgChain, sample.chainMaxUs, loadPercent, avgTick,
                                         sample.tickMaxUs, perPlayerUs,
                                         grid.entityCount(),
                                         m_serviceRegister.getService<StreamerService>().activePickupCount(),
                                         m_botCenter.x, m_botCenter.y, m_botCenter.z, staticNear, playersNear);

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST, "Нагрузочный прогон: результат", body, "Ок", ""),
                         [](DialogResponse, int, StringView)
                         {
                         });
}

void BenchmarkSystem::showResults(IPlayer &player)
{
    if (m_results.empty())
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Бенчмарк не дал результатов"));
        return;
    }

    const double top = m_results.front().nsPerCall;
    std::string body;
    for (const Result &result : m_results)
    {
        // Доля от самого тяжёлого кейса: показывает разрыв нагляднее абсолютных
        // наносекунд, которые зависят от машины.
        const int share = top > 0.0 ? static_cast<int>(result.nsPerCall / top * 100.0) : 0;
        body += fmt::format("{}\t{:.0f} нс\t{}%\n", result.name, result.nsPerCall, share);
    }
    body += fmt::format("--- база измерения ---\t{:.0f} нс\t-", m_baselineNs);

    m_dialogService.show(player,
                         makeDialog(DialogStyle_TABLIST_HEADERS,
                                    fmt::format("Бенчмарк: {} кейсов (запуск: {})", m_results.size(), m_lastRunBy),
                                    "Операция\tЦена вызова\tОт максимума\n" + body, "Повторить", "Закрыть"),
                         [this, playerId = player.getID()](DialogResponse response, int, StringView)
                         {
                             IPlayer *target = m_core.getPlayers().get(playerId);
                             if (!target || response != DialogResponse_Left)
                                 return;
                             runAll(*target);
                             showResults(*target);
                         });
}

#endif // GAMEMODE_BENCHMARK
