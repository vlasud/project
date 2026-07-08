# Персист наличных и оружия (Persistence)

Код (разведён по SRP на две независимые пары сервис+система):
`Src/Services/PlayerMoneyPersistService` + `Src/Systems/PlayerMoneyPersistSystem`
(наличные), `Src/Services/PlayerWeaponPersistService` +
`Src/Systems/PlayerWeaponPersistSystem` (оружие), `Src/Systems/PlayerAuthSystem`
(точка ПРИМЕНЕНИЯ — `m_pendingSpawnSetup`). Core-сервисы состояния:
`Src/Services/Core/PlayerMoneyService`, `Src/Services/Core/PlayerWeaponService`.
Схема БД: `Sql/schema_all.sql` (`player_money`, `player_weapon`).

## Что это

Раньше `PlayerAuthSystem::onPlayerSpawn` на первом реальном спавне после
логина хардкодом выдавал стартовый набор: `giveWeapon(24, 100)` (Desert Eagle),
`giveWeapon(31, 100)` (M4), `setMoney(1500)`. Наличные и оружие нигде не
сохранялись — при каждом новом входе игрок получал ровно этот набор заново,
независимо от того, что было на руках в прошлой сессии.

Теперь наличные (`PlayerMoneyService`) и оружие (`PlayerWeaponService`)
персистятся **per-account** в БД. Новый аккаунт стартует с **$0 и без
оружия** — стартовой эмиссии больше нет. То, что накоплено/подобрано за
сессию, переживает выход из игры и восстанавливается при следующем входе (с
оговоркой по оружию — см. «Каветы» ниже).

`PlayerMoneyService`/`PlayerWeaponService` остаются Core: чистое
рантайм-состояние без доступа к БД (см. `no-business-logic-in-core`). Весь
персист — бизнес-слой, разведён по SRP на два независимых сервиса+системы
(деньги не отвечают за оружие и наоборот): `PlayerMoneyPersistService`
(кэш) + `PlayerMoneyPersistSystem` (БД) для наличных,
`PlayerWeaponPersistService` (кэш) + `PlayerWeaponPersistSystem` (БД) для
оружия. Точка ВЫДАЧИ ОРУЖИЯ намеренно оставлена в `PlayerAuthSystem` — он уже
владеет таймингом «экипировка выдаётся ровно один раз, на первом спавне после
логина» (`m_pendingSpawnSetup`); наличные, в отличие от оружия, не требуют
этого тайминга (спавн их не сбрасывает) и применяются в `PlayerMoneyService`
сразу на загрузке — см. ниже.

## Таблицы

### `player_money`

| Колонка      | Тип         | Смысл                                             |
|--------------|-------------|----------------------------------------------------|
| `account_id` | BIGINT (PK) | аккаунт — один снимок наличных на аккаунт           |
| `cash`       | BIGINT (DEF 0) | серверный баланс наличных (`PlayerMoneyService::getMoney`), unsigned по природе |

Нет строки — новый аккаунт, наличные $0.

### `player_weapon`

| Колонка      | Тип         | Смысл                                             |
|--------------|-------------|----------------------------------------------------|
| `account_id` | BIGINT      | аккаунт                                             |
| `weapon`     | INT         | id оружия (SA-MP weapon id)                        |
| `ammo`       | INT (DEF 0) | патроны на момент сохранения (>= 0)                |

PK — `(account_id, weapon)`: одна строка на пару аккаунт-оружие. Нет строк —
аккаунт безоружен.

## PlayerMoneyPersistService / PlayerWeaponPersistService (кэш загрузки)

Не Core. Каждый сервис держит per-слот кэш СВОЕЙ половины того, что подгружено
из БД по старту сессии, и флаг «загрузка завершилась» — наличные и оружие
грузятся ДВУМЯ независимыми `selectQuery` (по одному на систему), приходят не
одновременно:

| Метод | Сервис | Семантика |
|----|----|----|
| `isMoneyLoaded(playerId)` / `cachedMoney(playerId)` | `PlayerMoneyPersistService` | загрузка наличных завершилась (значение УЖЕ записано в `PlayerMoneyService`, см. ниже) / загруженное значение |
| `areWeaponsLoaded(playerId)` / `cachedWeapons(playerId)` | `PlayerWeaponPersistService` | кэш оружия из БД пришёл (в `PlayerWeaponService` ЕЩЁ НЕ обязательно применён) / загруженный список `(weaponId, ammo)` |
| `areWeaponsApplied(playerId)` / `markWeaponsApplied(playerId)` | `PlayerWeaponPersistService` | `giveWeapon` реально прогнан по кэшу (спавн/late-observer) / взводит этот флаг — зовёт `PlayerAuthSystem` |
| `subscribeMoneyLoaded` | `PlayerMoneyPersistService` | наблюдатель: зовётся ПОСЛЕ КАЖДОЙ успешной загрузки наличных (в т.ч. штатной), см. «Гонка» ниже |
| `subscribeWeaponsLoaded` | `PlayerWeaponPersistService` | наблюдатель: зовётся ПОСЛЕ КАЖДОЙ успешной загрузки оружия (в т.ч. штатной), см. «Гонка» ниже |
| `loadMoney` / `reset` | `PlayerMoneyPersistService` | вызывает только `PlayerMoneyPersistSystem` (friend) |
| `loadWeapons` / `reset` | `PlayerWeaponPersistService` | вызывает только `PlayerWeaponPersistSystem` (friend) |

`reset(playerId)` на обоих сервисах — сброс кэша (у оружия — включая `applied`)
на коннекте/дисконнекте (как `InventoryService`), чтобы данные одного
аккаунта не утекли следующему игроку в том же `playerId`-слоте.

**Наличные и оружие асимметричны**, и это ключевое для гейта сохранения:
наличные `PlayerMoneyService` не сбрасывает на спавне, оружие `PlayerWeaponService`
чистит на КАЖДОМ спавне. Поэтому:

- наличные `PlayerMoneyPersistSystem::loadMoney` пишет в
  `PlayerMoneyService::setMoney` СРАЗУ на загрузке (как
  `InventorySystem::loadItems` — запись в авторитетный сервис ДО взвода флага),
  и `isMoneyLoaded` уже означает «в сервисе реальный баланс»;
- оружие на загрузке кладётся ТОЛЬКО в кэш (`PlayerWeaponPersistSystem::
  loadWeapons` -> `PlayerWeaponPersistService::loadWeapons`) — применить
  (`giveWeapon`) раньше логин-спавна нельзя, спавн его тут же стёр бы. Поэтому
  `areWeaponsLoaded` означает лишь «кэш пришёл», а факт реальной выдачи —
  отдельный флаг `areWeaponsApplied`, который взводит `PlayerAuthSystem` (см.
  «Применение» ниже).

## Загрузка (старт сессии)

Каждая система на `PlayerSessionService::subscribeStart` шлёт СВОЙ независимый
async-запрос:

- **Наличные** (`PlayerMoneyPersistSystem::loadMoney`) — один
  `selectQuery<unsigned long long>` по `account_id` (`player_money.cash`, нет
  строки → 0).
- **Оружие** (`PlayerWeaponPersistSystem::loadWeapons`) — один
  `selectQuery<vector<pair<uint8_t,int>>>` по `account_id` (все строки
  `player_weapon`); `weapon`/`ammo` читаются как `int64` и сужаются — битый
  ряд не роняет загрузку (как `InventorySystem::loadItems`), `weapon` вне
  `1..255` отбрасывается.

Оба колбэка на главном потоке проходят **serial-guard**
(`PlayerSessionService::get(playerId)->serial == serial` запроса) — в слоте мог
оказаться другой игрок/другая сессия, чужие деньги/оружие не должны прилететь.
После guard'а:

- **наличные** — колбэк СНАЧАЛА пишет `PlayerMoneyService::setMoney(cash)`
  (авторитетный сервис получает реальный баланс немедленно, независимо от
  того, прошёл ли уже логин-спавн), ЗАТЕМ `PlayerMoneyPersistService::
  loadMoney` в кэш — это взводит `isMoneyLoaded` и оповещает подписчиков;
- **оружие** — колбэк зовёт только `PlayerWeaponPersistService::loadWeapons`
  (кэш + `areWeaponsLoaded`, без записи в `PlayerWeaponService` — применение
  см. ниже).

## Применение (логин-спавн, `PlayerAuthSystem::m_pendingSpawnSetup`)

Единственная ВЫДАЧА ОРУЖИЯ — на первом реальном спавне после `finalize()`
(`m_pendingSpawnSetup[playerId] == true`, флаг гасится сразу).
`applyPersistedEquipment(player)`:

- если `isMoneyLoaded` (`PlayerMoneyPersistService`) — повторный (уже
  идемпотентный) `PlayerMoneyService::setMoney(cachedMoney)`: реальный баланс
  был записан ещё на загрузке, здесь лишь гарантия, что клиентский HUD
  синхронен, даже если загрузка догнала спавн между `setMoney` на загрузке и
  этим вызовом;
- если `areWeaponsLoaded` (`PlayerWeaponPersistService`) — `PlayerWeaponService::
  giveWeapon` для каждой пары из `cachedWeapons`, ЗАТЕМ `markWeaponsApplied` —
  взводит гейт сохранения оружия (см. «Сохранение» ниже).

Оружие применяется НЕ РАНЬШЕ логин-спавна: `PlayerWeaponService::onSpawn`
чистит инвентарь на каждом спавне (GTA теряет оружие на смерти) — выдай раньше,
и спавн бы это тут же стёр.

### Гонка: спавн раньше загрузки

Логин-спавн — отдельный клиентский round-trip (`finalize` → `setSpectating
(false)` → ожидание `onPlayerSpawn`), а загрузка — async-запрос к БД. В
подавляющем большинстве случаев БД успевает раньше (локальный round-trip
быстрее полного клиентского цикла спавна). Но код корректен и для обратного
случая:

- если на момент спавна `isMoneyLoaded`/`areWeaponsLoaded` ещё `false`,
  `applyPersistedEquipment` взводит `m_awaitingMoneyApply[playerId]` /
  `m_awaitingWeaponsApply[playerId]` (member-массивы `PlayerAuthSystem`);
- `PlayerAuthSystem` подписан на `PlayerMoneyPersistService::subscribeMoneyLoaded`
  и `PlayerWeaponPersistService::subscribeWeaponsLoaded`; когда загрузка
  приходит (штатно ИЛИ с опозданием), наблюдатель проверяет соответствующий
  `m_awaitingXxxApply` — если взведён, довершает выдачу (оружие — `giveWeapon`
  + `markWeaponsApplied`; наличные — повторный `setMoney`, реальный баланс в
  сервисе уже был записан загрузкой) и снимает флаг.

В штатном случае (загрузка раньше спавна) флаги `m_awaitingXxxApply` не
взводятся, и наблюдатель — no-op: двойной выдачи оружия/лишней записи нет.

`m_awaitingMoneyApply`/`m_awaitingWeaponsApply` сбрасываются в `resetState`
(коннект/дисконнект) — переиспользуемый `playerId`-слот не наследует чужое
ожидание.

## Сохранение (save-канал)

Каждая система на `PlayerSessionService::subscribeSave` (конец сессии И
периодический автосейв, см. `Docs/Autosave.md`) сохраняет СВОЮ половину:

- **Наличные** (`PlayerMoneyPersistSystem::persistMoney`) — если
  `isMoneyLoaded`: снимок `PlayerMoneyService::getMoney` → `INSERT ... ON
  DUPLICATE KEY UPDATE` (идемпотентный UPSERT) в `player_money`. Гейт
  корректен, потому что `isMoneyLoaded` взводится ПОСЛЕ того, как загрузка
  уже записала реальный баланс в `PlayerMoneyService` (см. «Загрузка» выше) —
  `getMoney()` под этим гейтом никогда не равен нетронутому дефолту чужой
  предыдущей сессии слота.
- **Оружие** (`PlayerWeaponPersistSystem::persistWeapons`) — если
  `areWeaponsApplied` (НЕ `areWeaponsLoaded`): снимок `PlayerWeaponService::
  getWeapons` (см. ниже) → REPLACE в `player_weapon`: `DELETE WHERE
  account_id=?` + `INSERT` каждой пары в ОДНОЙ транзакции (как
  `InventorySystem::persistItems`) — либо всё, либо ничего, `rollback` при
  сбое.

**Гейт критичен против дюп-лосса.** Наличные и оружие ведут себя по-разному, и
гейт под каждый рассчитан отдельно:

- если async-загрузка наличных ещё не завершилась (сбой БД / дисконнект до
  колбэка) — `isMoneyLoaded == false`, сохранение ПРОПУСКАЕТСЯ: иначе
  нетронутый дефолт (0, оставшийся от предыдущего occupant'а `playerId`-слота)
  затёр бы реальный баланс в БД раньше, чем он успел загрузиться;
- оружие теряет строки ещё в ОДНОМ дополнительном окне: `areWeaponsLoaded`
  становится `true`, как только кэш пришёл из БД, — но `PlayerWeaponService`
  реального оружия ЕЩЁ НЕ содержит (инвентарь пуст, применение — только на
  логин-спавне). Дисконнект/автосейв МЕЖДУ приходом кэша и спавном при гейте
  на `areWeaponsLoaded` сохранил бы пустой инвентарь поверх реального оружия
  аккаунта. `areWeaponsApplied` взводится ТОЛЬКО когда `giveWeapon` реально
  прогнан (см. «Применение» выше) — до этого момента сохранение оружия
  пропускается, старые строки в БД остаются нетронутыми.

Наличные и оружие гардятся НЕЗАВИСИМО (два разных сервиса+системы, могут
завершить загрузку/применение в разное время) — сохранение одного не
блокируется отсутствием второго.

## `PlayerWeaponService::getWeapons` (read-only геттер для персиста)

```cpp
void getWeapons(int playerId, std::vector<std::pair<std::uint8_t, int>> &out) const;
```

Единственное добавление в Core ради этой фичи — read-only снимок текущих
слотов (`weaponId, ammo`) для непустых слотов (`id != 0`); `ammo` клампится к
`>= 0` (`Slot::ammo` может уйти в отрицательный «долг» до порога ammo-hack —
сохранять отрицательное бессмысленно). `out` очищается перед заполнением,
bounds-safe. Никакой бизнес-логики — чистое чтение состояния, как
`getAmmo`/`hasWeapon`.

## Каветы

- **Оружие теряется на смерти (GTA).** `PlayerWeaponService::onSpawn` чистит
  инвентарь на КАЖДОМ спавне — восстанавливается оно ТОЛЬКО на логин-спавне
  (первом после входа), не на каждом респавне после смерти. Это была
  особенность и старого хардкода — модель тайминга не меняется, меняется
  только источник значений (БД вместо констант).
- **Снимок — не исторический максимум.** Сохраняется то оружие, что у игрока
  ЖИВЫМ прямо сейчас, на момент срабатывания save-канала (конец сессии или
  автосейв раз в 2 минуты). Если игрок мёртв/безоружен в момент автосейва — в
  БД уйдёт пустой набор, и следующий вход не вернёт оружие, добытое перед
  смертью, если оно не сохранилось до неё.
- **Наличные — снимок, не write-through.** Между сохранениями (автосейв раз в
  2 минуты + конец сессии) баланс живёт только в памяти — краш между
  автосейвами теряет изменения периода (как и остальные session-персисты,
  см. `Docs/Autosave.md`).

## Три правила

- **Читер-абуз:** серверный `getMoney`/`getWeapons` — источник данных для
  сохранения, клиенту не верим ни в чём. `accountId` берётся из
  `PlayerSessionService::Session` (не от клиента); `NO_ACCOUNT` — загрузка и
  сохранение — no-op (некуда). Гейты сохранения (`isMoneyLoaded` для наличных,
  `areWeaponsApplied` для оружия) не дают нетронутому дефолту/ещё не
  применённому кэшу затереть реальный баланс/инвентарь аккаунта в БД раньше,
  чем он реально оказался в рантайм-сервисе — закрывают окно дюп-лосса и на
  быстром дисконнекте сразу после коннекта, и на дисконнекте/автосейве в узком
  окне между приходом кэша оружия и логин-спавном. Serial-guard на ОБОИХ
  async-загрузках (наличные, оружие независимо) не даёт чужому балансу/оружию
  попасть в переиспользованный `playerId`-слот при быстром релоге.
  Отрицательный баланс невозможен (`getMoney` — unsigned, БД-значение
  клампится к 0 при чтении). Выдать оружия/патронов больше, чем было
  сохранено, нельзя — `giveWeapon` вызывается СТРОГО с загруженными из БД
  значениями, клиентского участия в выборе количества нет.
- **Краш-безопасность:** bounds-check `playerId` во всех методах
  `PlayerMoneyPersistService`/`PlayerWeaponPersistService`/`getWeapons`
  (включая `areWeaponsApplied`/`markWeaponsApplied`). Оба async-колбэка
  загрузки (в двух независимых системах) проверяют serial сессии И живого
  игрока (`getPlayers().get` + null) — дисконнект между отправкой запроса и
  приходом колбэка не разыменовывает мёртвый указатель. `reset` кэша (включая
  `applied` у оружия) на коннекте/дисконнекте в обеих системах и сброс
  `m_awaitingXxxApply` в `resetState` — переиспользуемый слот не наследует
  чужие данные/ожидания. Транзакция на сохранении оружия — либо всё, либо
  ничего (rollback при сбое), нет частичной записи. Нет ре-энтрантности:
  никаких таймеров, колбэки не отменяют друг друга.
- **Перф:** загрузка/сохранение — строго по событиям сессии (start/save), не
  per-tick. `getMoney`/`getWeapons`/чтение кэша — синхронно из памяти, O(1) и
  O(число слотов оружия) соответственно (маленькая константа, `MAX_WEAPON_SLOTS`).
  Все запросы к БД — асинхронно через `DatabaseManager`, главный поток не
  блокируется.
