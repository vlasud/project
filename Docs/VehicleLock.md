# Замок дверей личного транспорта

Замок — бизнес-фича (НЕ Core). Код: `Src/Services/VehicleLockService`,
`Src/Systems/VehicleLockSystem`. Управляется из `/car` -> «Текущая машина» ->
«Закрыть двери» / «Открыть двери» (см. `Docs/CarMenu.md`).

## Почему пер-игровой замок, а не глобальный `setLocked`

`VehicleService::setLocked` переставляет ОБЩИЙ `VehicleParams::doors` — клиент
блокирует вход ВСЕМ игрокам, включая владельца. Владелец, закрывший свою машину и
вышедший из неё, оказался бы заперт СНАРУЖИ: открыть замок можно только из
меню «Текущая машина», а оно требует сидеть В своей машине — вечный локаут без
стороннего вмешательства (админ-команда/рестарт).

SA-MP/open.mp умеет пер-игровые параметры машины: `IVehicle::setParamsForPlayer`
шлёт СВОЙ пакет `VehicleParams` конкретному игроку (см.
`ThirdParty/open.mp-sdk/include/Server/Components/Vehicles/vehicles.hpp`, рядом с
общим `setParams`/`getParams`). `VehicleService::setLockedForPlayer(vehicle, player,
locked)` — тонкая Core-обёртка, трогающая только `params.doors` для одного игрока;
общий `getParams()`/`setParams()` (двигатель/фары/сирена и т.д.) не задет.

**Замок сессионный — НЕ персистится.** Закрытая машина открывается заново с
дефолтом «не заперта» на каждой новой жизни экземпляра (в т.ч. просто перезаход
владельца — запись привязана к `vehicleId`, а не к `dbId`/аккаунту). Это
сознательное упрощение первой версии: цена ошибки низкая (машина видима только
владельцу/семье как «своя», посторонний в худшем случае не смог сесть один раз до
следующего запирания), а персист усложнил бы связку с респавном/пересозданием
экземпляра. Задел на будущее — см. «Будущее» ниже.

## Источник правды

`VehicleLockService` — `std::unordered_map<int, bool> m_locked` (`vehicleId` ->
заперта). Отсутствие записи == не заперта (дефолт открытой машины). Ключ по
`vehicleId`, а не `dbId`: замок относится к КОНКРЕТНОМУ живому экземпляру машины
(и у «вызванной» сессионной, и у припаркованной — обе имеют живой `vehicleId`, пока
существуют в мире; вызванная машина без `dbId`-парковки замком пользуется точно
так же).

## Политика доступа при закрытой машине

`allowedWhenLocked(vehicleId, accountId)` — кому НЕ заперто:

1. **Припаркованная (у дома/в семье)** — источник правды `ParkedVehicleService::
   byVehicleId(vehicleId)`:
   - владелец МАШИНЫ (`accountId == parked->ownerAccountId`) — всегда;
   - иначе, если расшарена (`familyId != NO_FAMILY`) — член ЭТОЙ семьи
     (`FamilyService::familyByAccount(accountId) == parked->familyId`);
   - иначе (личная, не владелец) — заперто.
2. **Обычная сессионная личная** (`VehicleService::getOwner == Owner::Player`) —
   владелец по `ownerId` тега (сессионный `playerId`), сверенный по `accountId`
   сессии (`PlayerSessionService::getAccountId`).
3. Прочие машины (`Faction`/`Work`/`None`, посторонний `vehicleId`) — сервис не
   держит для них записей (`isLocked` вернёт `false` — замка попросту нет);
   `/car` вообще не даёт закрыть чужую/нелегитимную машину (гейт «своя машина» в
   `CarMenuSystem` до вызова `toggle`).

`accountId` — ВСЕГДА серверный (из `PlayerSessionService`), не от клиента.
`NO_ACCOUNT` (не залогинен) не проходит НИКУДА.

## Применение (кто физически видит запертую дверь)

Клиент читает `VehicleParams` только на стрим-ине/апдейте — сервис обязан
проталкивать изменение сам, точечно, каждому застримленному игроку:

- **флип замка** (`setLocked`/`toggle`) — немедленно проходит по ВСЕМ онлайн-игрокам
  (`ICore::getPlayers().entries()`, O(MAX_PLAYERS), только на этом редком событии) и
  для тех, кому машина застримлена сейчас (`VehicleService::isStreamedInForPlayer` —
  обёртка `IVehicle::isStreamedInForPlayer`, сырой SDK бизнес не зовёт), ставит
  персональный `doors` по политике (`applyToPlayer`);
- **стрим-ин машины игроку** — `VehicleService::subscribeStreamedInForPlayer`
  (пробрасывает `VehicleEventHandler::onVehicleStreamIn` через `VehicleSystem`, см.
  ниже): ядро САМ пер-игровые `params` на повторном стрим-ине НЕ восстанавливает
  (`setParamsForPlayer` — заявление НА МОМЕНТ вызова, не персистентное состояние
  клиента) — без этой подписки только что подъехавший игрок увидел бы машину
  снова открытой. Дешёвый ранний выход: если для `vehicleId` вообще нет записи в
  `m_locked` (машину никогда не запирали), лишний RPC не шлётся;
- **share/unshare семье** (`ParkedVehicleService::subscribeReconcile`, стреляет на
  каждый флип `family_id`) — состав «свой» для УЖЕ закрытой машины меняется (новый
  член семьи должен тут же начать проходить, снятый с шеринга — перестать):
  `onReconcile(dbId)` резолвит `vehicleId` записи и, если она заперта, переприменяет
  всем застримленным. **Известный микро-гэп:** выход/кик РЯДОВОГО члена ИЗ семьи
  запись машины не флипает (`family_id` не меняется) — reconcile не стреляет, и у
  экс-члена, которому запертая семейная машина УЖЕ застримлена, дверь на клиенте
  остаётся открытой до ближайшего рестрима (отъехал/вернулся) или флипа замка.
  Затронута только клиентская дверь (сесть пассажиром): за руль экс-члена всё
  равно не пустит серверный driver-gate (`canDrive`), так что это косметическое
  окно, не дыра доступа;
- **уничтожение экземпляра** (`VehicleService::subscribeDestroyed`) — `onDestroyed`
  стирает запись `m_locked[vehicleId]`. Респавн (death/принудительный) экземпляр НЕ
  уничтожает (тот же `vehicleId`) — запись `m_locked` переживает респавн; при этом
  ядро на респавне САМО стримит машину ВСЕМ заново (`_respawn()` делает
  `streamOutForClient` каждому, кому она была видна, затем клиент повторно
  стримится) — значит `onVehicleStreamIn` сработает для всех и переприменит
  сохранённый замок автоматически, отдельного крючка на респавн не нужно.

## Core-примитивы (инфраструктура, без политики)

- **`VehicleService::setLockedForPlayer(IVehicle&, IPlayer&, bool locked)`** —
  `IVehicle::setParamsForPlayer` с одним изменённым полем (`doors`); прочие поля
  `VehicleParams` в дефолте конструктора (`-1` — «не менять» для большинства из них
  по контракту SDK).
- **`VehicleService::isStreamedInForPlayer(const IVehicle&, const IPlayer&)`** —
  тонкая обёртка `IVehicle::isStreamedInForPlayer`, чтобы бизнес не трогал сырой
  SDK-метод напрямую (конвенция проекта).
- **`VehicleService::subscribeStreamedInForPlayer(observer)`** + **`onVehicleStreamIn
  (IVehicle&, IPlayer&)`** — наблюдатель факта стрим-ина конкретному игроку;
  зовётся из `VehicleSystem::onVehicleStreamIn` (единственный подписчик SDK
  `VehicleEventHandler` в проекте, пробрасывает событие в сервис). Главный поток,
  событийно (стрим-ин НЕ per-tick).

Core здесь только даёт примитив и факт события — политику (кому можно, когда
переприменять) целиком решает бизнес (`VehicleLockService`/`VehicleLockSystem`).

## Регистрация

`VehicleLockService` (не Core) — `Src/Services/ServiceRegister.cpp`, ПОСЛЕ
`VehicleService`/`PersonalVehicleService`/`ParkedVehicleService`/`FamilyService` (все
нужны политике доступа). `VehicleLockSystem` — `Src/Systems/SystemRegister.cpp`,
сразу после `ParkedVehicleSystem` (bind зависит от него) и ДО `CarMenuSystem` (он
зовёт `VehicleLockService::isLocked`/`toggle`). В конструкторе системы —
`bind(...)` + подписки на `subscribeStreamedInForPlayer`/`subscribeDestroyed`
(`VehicleService`) и `subscribeReconcile` (`ParkedVehicleService`).

## API для `/car`

- **`isLocked(vehicleId)`** — для лейбла тумблера «Открыть двери» / «Закрыть двери».
- **`toggle(vehicleId)`** — флип + немедленное применение; возвращает НОВОЕ
  состояние (для текста сообщения об итоге). Закрыть/открыть может только владелец
  СВОЕЙ машины — гейт «своя машина» уже в `CarMenuSystem` (`ownCurrentVehicle`) ДО
  вызова; сервис дополнительно bounds/exists-safe (`setLocked` — no-op для
  несуществующей машины, страховка на случай гонки уничтожения между кликами).

## Три правила

- **Читер-абуз:** замок читает/переключает ТОЛЬКО `CarMenuSystem`, который до этого
  проверил «своя машина» (`ownCurrentVehicle`: личная сессионная по `ownerId ==
  playerId` ЛИБО припаркованная с `ownerAccountId == accountId` сессии) — чужую или
  расшаренную семье ЧУЖОГО владельца машину не закрыть/открыть. Политика доступа
  (`allowedWhenLocked`) читает ТОЛЬКО серверные записи (`ParkedVehicleService`/
  `VehicleService`/`FamilyService`), `accountId` — из сессии, не от клиента.
  `isStreamedInForPlayer`/`setParamsForPlayer` — обёртки Core, сырой SDK бизнес не
  вызывает напрямую.
- **Краш/UB:** `setLocked`/`toggle` — `exists`-guard через `VehicleService::get`
  (несуществующий `vehicleId` — no-op); `allowedWhenLocked`/`applyToPlayer`
  null-гардят указатели зависимостей (bind мог не отработать — оборонительно);
  `reapplyToStreamed` — null-гард `IVehicle*`/`ICore*`, итерация по `entries()` без
  мутации коллекции игроков внутри цикла; `onReconcile`/`onDestroyed` bounds-safe по
  `dbId`/`vehicleId` (несуществующая запись — no-op); подписки не трогают пул машин
  из колбэков смерти (замок — отдельные, независимые события). PER-TICK работы нет.
- **Перф:** `isLocked`/`setLocked` (без переприменения) — O(1) hash-lookup;
  `reapplyToStreamed` — O(MAX_PLAYERS), но ТОЛЬКО на редких событиях (клик по
  тумблеру, share/unshare) — не per-tick и не per-stream-in-конкретного-игрока (тот
  путь — O(1), не проходит по всем игрокам); `onStreamedInForPlayer` — ранний выход
  O(1) для машин, которые никогда не запирались.

## Будущее

- персист замка (доживает до рестарта/респавна) — потребует ключ по `dbId`
  припаркованной машины + восстановление на `spawnInstance`, для сессионных личных
  смысла меньше (машина и так исчезает на дисконнекте владельца);
- визуальная индикация «заперто» на клиенте (иконка/попап при попытке чужого войти)
  — сейчас чужой физически не входит (клиент сам не открывает запертую дверь SA),
  но серверного сообщения об отказе `VehicleLockService` не шлёт (в отличие от
  `ParkedVehicleSystem::onDriverGate`, который явно высаживает и объясняет отказ по
  ЗАПИСИ `Parked` — с замком эти два механизма независимы и не дублируют друг друга).
