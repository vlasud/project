# Верифицированные баги

Найдены веерной охотой (10 агентов по областям) + **адверсариальная верификация**
каждого кандидата (доказать воспроизводящим сценарием или опровергнуть). 15
кандидатов → 12 подтверждено (2 опровергнуто, 1 плейсхолдер отброшен).

**Легенда:** severity `critical|high|medium`, verdict `confirmed|plausible`.

## Статус независимого 2-го подтверждения (по 2 верификатора на баг)

Каждый баг перепроверен ДВАЖДЫ независимыми парами верификаторов (два полных
прохода + исходная охота = 3 независимых оценки). **Оба прохода дали идентичный
консенсус** — набор ниже стабилен:

- ✅ **9 солидно подтверждены** (оба confirmed, high): `db-pool-null`, `shutdown-loss`,
  `login-stale-cb` (все critical), `dialog-null`, `hp-ratchet-death`, `teleport-grace`,
  `wskill-gate` (high), `editor-race`, `gzone-race` (medium).
- ⚖️ **1 спорный** — `ammo-debt` (#5): код-асимметрия реальна (нет `removeWeapon`), но
  последствия переоценены — AntiCheatSystem кикает при 5 нарушениях/60с (~10с, не
  «бесконечно»), а авторизация урона (`validateGiveDamage`) ammo-агностична. Severity
  скорее ниже заявленной. Перепроверить человеку: полностью ли `validateGiveDamage` не
  зависит от ammo/владения; надёжно ли кик при паузном темпе стрельбы.
- ❌ **2 опровергнуты повторной проверкой:**
  - `textdraw-uaf` (#8): механизм реален, но недостижим — `m_globalClickHandlers`
    никогда не заполняется (мёртвый код), единственный per-player обработчик не делает
    self-clear/replace в том же кадре. Латентный дефект (стоит copy-before-invoke как
    defense-in-depth), но НЕ воспроизводимый баг.
  - `chat-colorcodes` (#11): ядро open.mp уже чистит цветокоды — `PlayerTextRPCHandler`
    при `chat_input_filter=true` (реально в `Server/config.json`) regex'ом вырезает
    `{RRGGBB}` ДО диспетчеризации. Сценарий недостижим при текущем конфиге. Остаточный
    риск только если админ выключит фильтр.

---

## 🔴 CRITICAL — исправлено

### 1. ✅ DB-пул выдаёт слот с несозданным `mysqlx::Session` → краш
`DatabaseManager.cpp:41-63,21-34` · `StaticPool.h:20-26,71-80` · `null-deref`
Если на старте 1 из 16 подключений к MySQL упало, `SessionWrapper::initialize` вернул false (m_session=nullptr), но `DatabaseManager::initialize` проверяет только `opened==0`. `StaticPool` кладёт ВСЕ 16 индексов в доступные и не умеет изымать битый → рано или поздно `getSchema()` разыменует nullptr на воркере (не `std::exception`, `catch(...)` в ThreadPool не ловит) → краш процесса.
**Фикс (исправлено):** `StaticPool::init(initFn)` наполняет `m_availableIndices` только для слотов, где `initFn` вернул true (`DatabaseManager::initialize` передаёт `wrapper.initialize()`) — провалившийся слот навсегда исключён из выдачи; `SessionWrapper::getSchema()` при `m_session==nullptr` бросает `std::runtime_error` (ловит существующий catch → errorCallback) как защита на крайний случай. Гейт `opened==0` сохранён.

### 2. ✅ Отложенные (backpressure) DB-запросы теряются на shutdown → потеря данных
`ThreadPool.cpp:31-54,57-118` · `DatabaseManager.cpp:79-96` · `data-loss`
На остановке при массовом сохранении (>16 одновременных запросов) часть лежит в очереди из-за исчерпания пула. `shutdown()` убивает воркеров, зовёт `flush()` ОДИН раз; колбэк успешной задачи (`releaseAndPump`) добавляет отложенную в `m_tasks` через `addTask`, но воркеров уже нет и второго flush не будет → финальные сохранения баланса/инвентаря навсегда остаются невыполненными.
**Фикс (исправлено):** `shutdown()` после join/clear воркеров крутит цикл до фикс-точки: `runQueuedTasksSync()` (синхронно `run()` всего, что накопилось в `m_tasks`, на вызывающем потоке) → `flush()` (раздаёт `call()/fail()`, что может реентрантно доложить в `m_tasks` через `releaseAndPump`) → повтор, пока `m_tasks` пуст и `m_pendingCallbacks==0`. Завершимость: `releaseAndPump` выпускает из backpressure-очереди не больше одной задачи на каждую завершённую — цепочка строго конечна. `addTask` при `m_stop && m_threads.empty()` больше не теряет молча — логирует warning.

### 2b. ✅ Логин: stale ThreadPool-колбэк авто-аутентифицирует чужого игрока (обход пароля)
`PlayerAuthSystem.cpp:279-335` · `race / auth-bypass`
`task.func` захватывает пароль A корректно, но `task.callback` — только `[this, playerId]`. Пока идёт медленный argon2-verify, A дисконнектится, слот N переиспользуется под B (open.mp reuse), B грузит свой аккаунт в `m_loginData[N]`. Stale-колбэк A: `verified=true`, `get(N)` = живой B, null-check проходит → `sessionService.start(B, аккаунт B)` + `finalize(B)` → **B входит в свой аккаунт БЕЗ пароля**. Гейт пароля обойдён для любого, кто наследует слот в этом окне.
**Фикс (исправлено):** `requestConnectionVersion = m_connectionVersionService.getVersion(playerId)` захватывается при постановке `ThreadPool::Task`, первой строкой `task.callback` сверяется с текущей версией — при несовпадении тихий выход (паттерн зеркалит уже существующие в этом же файле DB-колбэки).

## 🟠 HIGH

### 3. ✅ `PlayerDialogSystem::initialize` — null-deref, если нет `Dialogs.dll`
`PlayerDialogSystem.cpp:12-13` · `null-deref / crash`
`queryComponent<IDialogsComponent>()->getEventDispatcher()` без null-check → если Dialogs.dll не в components, краш всего гейммода на старте (соседи GangZone/Timer/Pickup именно тут делают проверку).
**Фикс (исправлено):** null-check `IDialogsComponent`; при nullptr — `LogManager::log(Error, ...)` и обработчик не регистрируется (как у GangZone/Timer/Pickup).

### 4. ✅ Сброс ammo-debt в 0 → бесконечный обход ammo-hack, оружие не отбирается
`PlayerWeaponService.cpp:245-253` · `cheat-abuse`
С серверным ammo=0 и честным темпом: каждые 11 выстрелов 10 проходят без флага (`ammo < -AMMO_DEBT` строго `<-10`), 11-й обнуляет долг, но оружие НЕ снимается (в отличие от unowned-ветки) → цикл бесконечен, урон идёт.
**Фикс (исправлено):** долг клампится к полу `-AMMO_DEBT` (вместо обнуления) — следующий нелегальный выстрел ретриггерит детект без окна прощения; оружие не снимаем (severity низкая, античит кикает по накоплению нарушений).

### 5. ✅ Ратчет HP: ветка первого совпадения суммы не объявляет смерть при `health<=0`
`PlayerHealthService.cpp:368-395` · `cheat-abuse`
После легальной смены HP без обнуления клиент шлёт `reportedHealth=0`; ветка ратчета присваивает `st.health` и НЕ вызывает `enterDying` (в отличие от соседней ветки 423-424) → смерть не фиксируется.
**Фикс (исправлено):** после cap-клэмпа добавлен `if (st.health <= 0.0f) enterDying(player);` — симметрично ветке-близнецу.

### 6. ✅ Grace телепорта продлевается бесконечно тактированием sync → детект teleport-hack подавлен
`PlayerLocationService.cpp:292-327` · `cheat-abuse`
После серверного `teleport()` клиент игнорирует RPC и шлёт sync ровно каждые >2с (PAUSE_GAP) → resumedFromPause-ветка каждый раз продлевает grace → teleport-hack навсегда не детектится и позиция не ресинкается.
**Фикс (исправлено):** введён `teleportIssuedAt` (ставится один раз в `forceTo`/при повторной выдаче, НЕ трогается pause-веткой); таймаут грейса гейтится по `now - teleportIssuedAt`, а сброс грейса в resumedFromPause убран. Обычный pause-ресинк (не телепорт) не затронут.

### 7. ✅ `/wskill` дев-команда без гейта прав (доступна всем)
`WeaponSkillSystem.cpp:29-31` · `cheat-abuse`
4-й аргумент `add()` = `{}` → `PermissionSpec::Kind::None` → любой игрок вызывает. *(тот же класс, что пофикшенный `/mute`.)*
**Фикс (исправлено):** 4-й аргумент `add()` → `PermissionSpec::admin(AdminService::DEVELOPER_LEVEL)` + `#include Services/AdminService/AdminService.h` (как в соседних дев-Core-системах — они используют статическую `DEVELOPER_LEVEL`, инстанс не тянут).

### 8. `TextDrawService`: клик-обработчик без копии → UAF при self-clear/replace *(plausible)*
`TextDrawService.cpp:385-389,400-404` · `memory-safety`
`it->second(player)` по ссылке на узел map; self-clear (`erase`) или self-replace внутри обработчика → dangling. Дефект реален и отклоняется от конвенции проекта (5 др. сервисов копируют колбэк), но живого вызывающего, триггерящего его, сейчас нет.
**Фикс:** `ClickHandler handler = it->second; handler(player);` (как в `handleCancelSelection`).

### 12. ✅ Кошельки работ: `withdraw` чистым UPDATE теряет первое списание нового аккаунта → дюп на релоге
`BusWalletService.cpp` · `PortWalletService.cpp` · `HaulerWalletService.cpp` · `money-dup / data-loss`
Класс бага в ТРЁХ близнецах-кошельках (bus/port/hauler). `withdraw` обнулял кэш и слал `UPDATE {wallet} SET balance=balance-N WHERE account_id=?`. У нового аккаунта строки кошелька может ещё не быть: первый в жизни `add` — тоже async в пуле сессий, и `withdraw` может обогнать его INSERT. Тогда UPDATE задевает 0 строк, списание теряется; когда `add`-INSERT наконец вставит `+N`, в БД останется `+N` без вычета → на следующем логине игрок получает дубль суммы.
**Фикс (исправлено):** `withdraw` во всех трёх сервисах переведён на UPSERT-совместимое относительное списание `INSERT INTO {wallet}(account_id, balance) VALUES(?, -N) ON DUPLICATE KEY UPDATE balance = balance + VALUES(balance)` (тот же стиль, что уже был у `add`). INSERT гарантирует строку; относительные -N/+N коммутируют, итог сходится к верному независимо от порядка async-записей (баланс временно отрицателен, пока не придёт +N от `add`). Обнуление кэша и выдача наличных на руки — без изменений.

## 🟡 MEDIUM

### 9. ✅ Replace-load гонка в `EditorSystem` стирает размещённое во время async-чтения
`EditorSystem.cpp:3475-3532` · `data-loss`
`/editor`→Load→«Заменить сцену»: async-чтение без лока/loading-диалога; размещённое за это время затирается `clearScene` в колбэке.
**Фикс (исправлено):** per-`EditorState` флаг `asyncOpPending` (ставится перед addTask в load/save/list, снимается в callback И errorCallback ДО мутаций сцены); пока стоит — create/place/duplicate/load/save/list игрока отклоняются с «идёт операция с файлом».

### 10. ✅ Replace-load гонка в `GangZoneEditorSystem` стирает зоны, созданные во время чтения
`GangZoneEditorSystem.cpp:855-887` · `data-loss`
Аналогично: `/gzone load` → колбэк `clearZones` затирает зоны, созданные любым админом за время чтения.
**Фикс (исправлено):** монотонный `m_revision` в `GangZoneService` (растёт на add/remove/clear/setRect/setColour/priority); редактор захватывает его при постановке load-задачи и сверяет в колбэке ПЕРЕД `clearZones` — при изменении устаревшая загрузка отменяется с «зоны изменились, загрузка отменена», зоны не затираются.

### 11. ✅ `ChatSystem` рассылает сырой текст чата без чистки цветовых кодов — НЕ ВОСПРОИЗВОДИТСЯ
`ChatSystem.cpp` · `cheat-abuse`
Проверено в игре: игрок отправил `color {FF0000}RED and ~r~tilde`, окружающие получили `- color  RED and -r-tilde : Lo_Vlasud[0]`. Инъекции нет, и защита двойная:
* `Encoding::neutralizeLine(buffer, lineSize)` в `ChatSystem` прогоняется по ВСЕЙ собранной строке (и по тексту, и по нику) — `{`/`}` становятся `(`/`)`, `~` становится `-`, управляющие байты пробелом. Тильды в замере превратились в дефисы именно здесь;
* фигурные скобки до сервера вообще не доехали (в `[chat]`-логе их уже нет) — их срезает движок: в `Server/config.json` включён `chat_input_filter`.

Пункт описывал состояние до появления `neutralizeLine` и устарел. Опираться на один `chat_input_filter` при этом нельзя: он настройка сервера и может быть выключен, поэтому нейтрализацию строки в коде оставляем.

---

## Опровергнуто на верификации
- «path traversal в имени файла» (был ранее) — blacklist `/`,`\` уже не даёт выйти из каталога; реальным был лишь `:` ADS-gap (закрыто общим whitelist).
- Плейсхолдер-кандидат «Test» — без кода/локации.

*Все подтверждённые — с воспроизводящим сценарием. Документ — снимок; вычёркивать по мере фикса.*
