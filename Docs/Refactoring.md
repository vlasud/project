# Бэклог рефакторинга

Ранжированный список мест для рефакторинга по всему `Src/`. Собран веерным
анализом (11 агентов-сканеров по областям кода + сквозной охотник за
дублированием, 72 сырых находки → дедуп/группировка/ранжирование).

**Легенда:** `[impact/effort]` — impact `high|med|low`, effort `S|M|L`.
Внутри тем отсортировано по ROI (impact÷effort).

> ⚠️ Часть находок — **не стиль, а баги** (абуз-векторы, UB, потеря данных).
> Их стоит чинить как дефекты, независимо от косметического рефактора.

---

## ✅ Сделано (Тема 1 — баги, верифицировано + review + QA)

Прогнано через адверсариальную верификацию (каждый баг подтверждён по коду перед фиксом):
- ✅ **`/mute`/`/unmute`** — навешен `PermissionSpec::admin(1)` (было доступно всем). *(подтверждено, critical)*
- ✅ **Санитайзер имени файла** — вынесен общий `Utils/FileNameSanitizer.h` (whitelist `[A-Za-z0-9_-]`, ≤64), подключён во всех 5 редакторах + `CameraService` на save И load. *Переоценка:* «path traversal в произвольные файлы» НЕ подтвердился (blacklist `/`,`\` уже не давал выйти из каталога) — реальный gap был `:` (ADS на NTFS, тот же каталог) + дедуп/консистентность. *(partial → hardening/dedup)*
- ✅ **`FactionService`** — 3 многошаговые записи (`setMember`/`appointLeaderByAccount`/`deleteRank`) обёрнуты в транзакции с rollback. *(подтверждено, medium)*
- ✅ **`CameraService::loadPathFromFile`** — потерянный error-callback (moved-from) починен через `shared_ptr`. *(подтверждено)*
- ✅ **`ServiceRegister::getService`** — UB на nullptr в release заменён контролируемым падением. *(подтверждено, high)*

Пункт про **blanket bounds-check `playerId`** — сознательно НЕ делали: при текущих вызовах id приходит из `IPlayer::getID()` (всегда валиден), проверки в основном избыточны; ценность — консистентность, не живой баг (обсуждено с владельцем).

---

## С чего начать дальше (ранжировано по ROI)

1. ✅ **Сделано** — `u()`/`utf8Tocp1251` вынесен в `Encoding.h` (`inline u(std::string_view)`), удалены 38 локальных копий.
2. ✅ **Сделано** — общий `makeDialog`/`makeDialogCp1251Body` в `Services/Core/PlayerDialogService/MakeDialog.h`; удалены 12 локальных копий, подтянуты Faction/Election/Admin/Menu (все их ручные Dialog-блоки оказались drop-in).
3. ✅ **Сделано** — `finiteOrZero`/`sanitize(Vector3)`/`clampFinite` в `Utils/Sanitize.h`; удалены копии в 8 сервисах (в т.ч. member-методы `sanitizePosition` в MapIcon/Checkpoint).
4. ✅ **Сделано** — дубль клэмпа HP к кэпу вынесен в приватный `clampToCapAndForce`, зовётся из обеих веток `verify` (семантика бит-в-бит).
5. `[high/M]` Вынести `VehicleFuelRestoreTracker` для Personal/Parked vehicle.
6. `[high/M]` Шаблонный `PresetFileService<T>` для async save/load/list в 5 editor-системах.
7. `[high/L]` Абстрагировать account-scoped персист (serial-guard + `m_loaded`) в миксин/хелпер.
9. `[high/M]` Вынести `VehicleFuelRestoreTracker` для Personal/Parked vehicle (устранить параллельный дрейф).
10. `[high/M]` Шаблонный `PresetFileService<T>` для async save/load/list в 5 editor-системах.
11. `[high/L]` Абстрагировать account-scoped персист (serial-guard + `m_loaded`) в миксин/хелпер.

---

## Тема 1. Дефекты корректности и безопасности (чинить как баги)

| Находка | i/e | Место | Направление |
|---|---|---|---|
| `/mute`/`/unmute` без проверки прав (Kind::None — всем) | high/S | `ChatSystem.cpp:56-93` | admin-gate через `AdminService`; лучше вынести модерацию из Core-чата |
| Санитайзер имени файла: blacklist вместо whitelist, без ре-валидации на load | high/S | Editor/GangZoneEditor/TextDrawEditor/DebugCamera vs AttachmentEditor (5 мест) | whitelist `[A-Za-z0-9_-]` max 64 → `Utils/FileNameSanitizer.h`, звать и на загрузке |
| `FactionService`: многошаговые записи БД без транзакции | high/M | `FactionService.cpp:272-280,540-549,753-767` | `startTransaction/commit+rollback` (образец SpawnChoice/Family) |
| `CameraService::loadPathFromFile`: errorCallback копирует уже перемещённый `onLoaded` | med/S | `CameraService.cpp:100-137` | `shared_ptr<LoadHandler>` между callback и errorCallback |
| `ServiceRegister::getService` — UB-ссылка на nullptr в release | med/S | `ServiceRegister.h:15-28` | контролируемое падение: `Fatal`+`std::abort()` или исключение |
| Пропущенный bounds-check `playerId` → OOB/UB | med/S | `PlayerAuthService`, `PlayerActivityService`, `PlayerKeyService`, `PlayerConnectionVersionService` + reset()/setMoney/attach… | общий `isValidPlayerId` во всех непокрытых методах |

## Тема 2. Вынос сквозных утилит в общие заголовки (лучший ROI дедупа)

| Находка | i/e | Место | Направление |
|---|---|---|---|
| `u() = Encoding::utf8Tocp1251` продублирован дословно | high/S | **38 файлов** | `inline std::string u(std::string_view)` в `Encoding.h`; перегрузка под готовый cp1251 (TextDrawEditor) |
| Ручной гард `playerId<0 \|\| >=MAX_PLAYERS` | high/S | **~128 вхождений / 35 файлов** | `constexpr isValidPlayerId(int)` + free `onlinePlayer(ICore&,int)` |
| ✅ `makeDialog(...)` с u()-конвертацией | med/S | 12 бизнес- + 9 editor-систем | общий `makeDialog` + `makeDialogCp1251Body` под готовый body; Faction/Election/Admin/Menu подтянуты |
| ✅ `finiteOrZero` + `sanitize(Vector3/Vector2)` переизобретены | med/S | 8 сервисов | `Utils/Sanitize.h` (`finiteOrZero`/`sanitize(Vector3)`/`clampFinite`) |
| `sanitizeText` дублирован дословно | med/S | `TextDrawService:71-99`, `GameTextService:14-38`, `TextLabelService:20-43` | общий хелпер с флагами allowNewline/tildeParity/emptyFallback |
| `PortWalletService` ≈ `BusWalletService` ≈ `HaulerWalletService` дословно (иная таблица) | med/S | `Services/PortWalletService`, `Services/BusWalletService`, `Services/HaulerWalletService` | общий `WorkWalletService`/`AccountWallet<Tag>` (persist-кошелёк работы: add/withdraw/load/reset + таблица параметром); сейчас НЕ обобщено сознательно (три работы — всё ещё ранняя стадия, риск задеть рабочие фичи) |
| Депо-каркас `HaulerJobService`/`HaulerJobSystem` ≈ `BusJobService`/`BusJobSystem` (слоты/пре-сток/резерв/FIFO-очередь/окно посадки/маркер/гейт руля/пере-сток) | high/L | `Services/HaulerJobService` + `Systems/HaulerJobSystem` vs `Services/BusJobService` + `Systems/BusJobSystem` | вынести общий `DepotService`/`DepotSystem<Job>` (slots/prestock/reserve/queue/boarding-window/driver-gate/restock параметризовать job-колбэками); НЕ обобщено сознательно — маршрут у работ РАЗНЫЙ (автобус = кольцевой race, развозчик = многофазный цикл езда↔пешая погрузка/разгрузка), а автобус — уже рабочая гейтед-фича, обобщение с гарантией «без изменения поведения автобуса» — большой риск регресса; депо-часть дублирована по образцу (решение владельца/ТЗ) |
| `asciiLower`/`iequals`, `ceilSecondsUntil` | low/S | Command/Animation, Admin/Report | `Utils/Ascii.h`, `TimeUtils::ceilSecondsUntil` |

## Тема 3. Персистентность и инфраструктура БД

| Находка | i/e | Место | Направление |
|---|---|---|---|
| Каркас «load на subscribeStart + serial-guard + m_loaded + save на subscribeSave» | high/L | 8+ сервисов (Inventory, PlayerMoneyPersist, PlayerWeaponPersist, WeaponProficiency, SpawnChoice, PersonalVehicle, PortJob, Faction…) | шаблонный `AccountScopedCache<T>`/`SerialGuardedLoad<T>` — забыть гард = чит-вектор |
| `DatabaseManager`: session-acquire/enqueue дублируется | med/M | `DatabaseManager.cpp:99-137`, `.h:118-169` | `acquireSessionOrEnqueue`, `logAndForward` |
| Захардкоженные креды MySQL | med/M | `DatabaseManager.cpp:14-18` | в конфиг/env, читать в `initialize()` |

## Тема 4. Гард-преамбулы и лукапы в бизнес-системах

| Находка | i/e | Место | Направление |
|---|---|---|---|
| `VehicleService`: bounds-check машины вручную в ~15 методах (местами уже расходятся) | med/M | `VehicleService.cpp` (get/health/owner/fuel/colour/…) | `validVehicle(int)` / `stateFor(int)` |
| `FactionSystem`: serial-guard + «target ещё в фракции» ~8 раз | med/M | `FactionSystem.cpp:538,576,649,747,848,883,1096,1236,1278` | `revalidateTarget(...)` |
| `AdminSystem`: target-lookup + иерархия старшинства в ~7 командах | med/M | `AdminSystem.cpp:487-786` | `resolveSubordinate(actor,targetId,…)` |
| `CarMenuSystem`: bounds-check carIndex + «недоступна» 8+ раз | med/M | `CarMenuSystem.cpp:180…829` | `validCar(player,carIndex)` |
| `HomeMenuSystem`: гард «авторизован + свой дом» в 8 хендлерах | med/S | `HomeMenuSystem.cpp:52…482` | `ownedHouseOrReject(player)` |

## Тема 5. Editor/Debug системы: файловый IO, диспетчеризация, god-файл

| Находка | i/e | Место | Направление |
|---|---|---|---|
| Async save/load/list JSON-пресетов через ThreadPool дублируется | high/M | 5 систем (Attachment/GangZone/TextDraw/DebugCamera/Editor) | шаблонный `PresetFileService<T>` с session-token, лимитом размера, анти-traversal |
| `EditorSystem`: per-EntityType диспетчеризация в ~7 функциях | med/L | `EditorSystem.cpp` (applyTransform/delete/clear/snap/duplicate/probe/showMain) | таблица стратегий `EntityOps` / подклассы |
| `showObjectList/ActorList/VehicleList/PickupList/CheckpointList` — копипаст | med/M | `EditorSystem.cpp:3119-3350` | общий `showEntityList(...)` |
| `EditorSystem.cpp` — god-файл ~3700 строк на 5 доменов | med/L | `EditorSystem.cpp`, `.h:34-212` | декомпозиция по типам; `INTERIOR_SPOTS[]` (139) → `Data/InteriorSpots.h` |
| `GangZoneEditor::onPlayerUpdate` дублирует axis-delta из `EditorSystem` | low/M | `GangZoneEditorSystem.cpp:165-273` | общий «axis-delta из keys + step» |

## Тема 6. Внутренний дедуп Core-сервисов (health/weapon/state/streamer/location/vehicle)

| Находка | i/e | Место | Направление |
|---|---|---|---|
| ✅ Дубль блока клэмпа HP к кэпу в `verify()` — дословно дважды | high/S | `PlayerHealthService.cpp:387-393` и `416-422` | `clampToCapAndForce(...)` |
| Идиома forceSync (lastChange/confirmed=false/setHealth[+Armour]) ×4 | med/S | `PlayerHealthService.cpp` (setHealth/setArmour/setMaxHealth/applyDamage) | приватный `forceSync(...)` |
| `validateMod` и `validatePaintJob` — почти дублирующиеся тела | med/S | `VehicleService.cpp:1137-1249` | `validateModShopTuning(predicate,label)` |
| `PlayerLocationService::verify` — god-метод ~175 строк, 8 сценариев | med/L | `PlayerLocationService.cpp:222-397` | выделить `checkBypass/checkPendingTeleport/checkPauseResume/checkSpeedLimit` |
| `StreamerService`: 3 копии merge-diff + 3 копии lower_bound-erase | med/L | `StreamerService.cpp:376-510; 82-278` | шаблонный merge-diff + хелпер снятия defId |
| Прочий boilerplate (11 Observer-списков в VehicleService, config get/set в WorldService, touch(), TimedFlag) | low/M | VehicleService.h:163-258; WorldService; PlayerWeaponService | `Observable<Args...>`, `setConfigFlag`, `touch(State&)`, `AntiCheatService::recordIf` |

## Тема 7. Personal/Parked vehicle: топливо и лукапы

| Находка | i/e | Место | Направление |
|---|---|---|---|
| Восстановление топлива после несанкц. смерти дублировано в 2 системах | high/M | `PersonalVehicleSystem.cpp:290-378`, `ParkedVehicleSystem.cpp:110-182` | `VehicleFuelRestoreTracker` |
| Мелкий дедуп (`clampFuel`, unpark DELETE, `findOwnedIndexByVehicleId`, eraseFromMultimap) | low/S | Parked/Personal vehicle сервисы/системы | вынести в `VehicleService`/общие хелперы |

## Тема 8. Консистентность, dead-code, Core/бизнес

| Находка | i/e | Место | Направление |
|---|---|---|---|
| Дев-команды с чатом/диалогами в Core-системах (нарушение «нет бизнес в Core») | med/M | `SpectateSystem.cpp:27-71`, `WeaponSkillSystem.cpp:29-69` | вынести регистрацию команд/меню в не-Core системы |
| Непоследовательная обработка отсутствия компонента при `queryComponent` | low/S | `VehicleSystem.cpp:38-40` vs Checkpoint/Pickup/… | единый паттерн лог+early-return |
| `OneShotEvent`-примитив дублирован | low/M | FamilyService, HouseService | шаблон `OneShotEvent<Args...>` |
| Dead-code / устаревшие хвосты | low/S | PlayerMoneySystem (unused m_antiCheatService), PlayerAuthSystem (пустой onPlayerKeyStateChange), закомм. регистрации | убрать/пояснить |
| Прочие точечные дедупы (demoteLeaders, forEachWatcherOf, proximity broadcast) | low/S | FactionService, SpectateService, Chat/RoleplayChat | вынести общие хелперы |

---

*Документ — снимок анализа; при работе по пунктам вычёркивать/актуализировать.*
