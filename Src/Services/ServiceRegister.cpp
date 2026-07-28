#include "Services/ServiceRegister.h"

#include "Services/AdminService/AdminService.h"
#include "Services/BanService/BanService.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/AttachmentService/AttachmentService.h"
#include "Services/BankService/BankService.h"
#include "Services/BusJobService/BusJobService.h"
#include "Services/BusWalletService/BusWalletService.h"
#include "Services/HaulerJobService/HaulerJobService.h"
#include "Services/HaulerWalletService/HaulerWalletService.h"
#include "Services/ElectionService/ElectionService.h"
#include "Services/FactionService/FactionService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/HouseService/HouseService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Services/InventoryService/InventoryService.h"
#include "Services/JobDismissService/JobDismissService.h"
#include "Services/JobWalletService/JobWalletService.h"
#include "Services/MedicJobService/MedicJobService.h"
#include "Services/MedicWalletService/MedicWalletService.h"
#include "Services/PhoneService/PhoneService.h"
#include "Services/PlaceCatalogService/PlaceCatalogService.h"
#include "Services/TaxiJobService/TaxiJobService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
#include "Services/PortJobService/PortJobService.h"
#include "Services/PortWalletService/PortWalletService.h"
#include "Services/VehicleLockService/VehicleLockService.h"
#include "Services/Core/AudioService/AudioService.h"
#include "Services/Core/CameraService/CameraService.h"
#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/Core/ClassSelectionService/ClassSelectionService.h"
#include "Services/Core/GameTextService/GameTextService.h"
#include "Services/Core/GangZoneService/GangZoneService.h"
#include "Services/Core/GridService/GridService.h"
#include "Services/Core/PlayerActivityService/PlayerActivityService.h"
#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"
#include "Services/PlayerAuthService/PlayerAuthService.h"
#include "Services/PlayerMoneyPersistService/PlayerMoneyPersistService.h"
#include "Services/PlayerWeaponPersistService/PlayerWeaponPersistService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/PlayerSpawnService/PlayerSpawnService.h"
#include "Services/Core/PlayerChatService/PlayerChatService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/MovingObjectService/MovingObjectService.h"
#include "Services/Core/NavigationLockService/NavigationLockService.h"
#include "Services/Core/NicknameService/NicknameService.h"
#include "Services/Core/ObjectEditService/ObjectEditService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerKeyService/PlayerKeyService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerSavedLocationService/PlayerSavedLocationService.h"
#include "Services/Core/PlayerSkinService/PlayerSkinService.h"
#include "Services/PlayerPersonalSkinService/PlayerPersonalSkinService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/PlayerVelocityService/PlayerVelocityService.h"
#include "Services/ReportService/ReportService.h"
#include "Services/SpawnChoiceService/SpawnChoiceService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/Core/ScreenTimerService/ScreenTimerService.h"
#include "Services/Core/WeaponSkillService/WeaponSkillService.h"
#include "Services/WeaponProficiencyService/WeaponProficiencyService.h"
#include "Services/Core/SpectateService/SpectateService.h"
#include "Services/Core/StreamerService/StreamerService.h"
#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Services/Core/TextLabelService/TextLabelService.h"
#include "Services/Core/TimerService/TimerService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/Core/WorldService/WorldService.h"

void ServiceRegister::registerServices()
{
    registerService<TimerService>();
    registerService<NicknameService>();
    registerService<PlayerConnectionVersionService>();
    registerService<PlayerLocationService>();
    registerService<PlayerSavedLocationService>();
    registerService<PlayerStateService>();
    registerService<PlayerVelocityService>();
    registerService<PlayerWeaponService>();
    registerService<WeaponSkillService>();
    // Кастомная прогрессия владения оружием (бизнес-стат аккаунта, не Core):
    // без зависимостей; PlayerWeaponSystem (хук выстрела) и WeaponProficiencySystem
    // (persist) берут её из регистра.
    registerService<WeaponProficiencyService>();
    registerService<VehicleService>();
    // Личный транспорт игрока (бизнес-фича, не Core): источник правды о владении
    // личными машинами онлайн. ПОСЛЕ VehicleService — зависит от него
    // (PersonalVehicleSystem связывает их через bind в конструкторе). Право владения
    // персистится в БД (personal_vehicle, write-through); машина-сущность сессионная.
    registerService<PersonalVehicleService>();
    // Указатель-на-машину (бизнес-фича, не Core): единый владелец персонального
    // красного чекпоинта к машине игрока. Переиспользуется парковкой, /car и /gps.
    // Привязывается к CheckpointService через bind в VehicleWaypointSystem
    // (реестр конструирует сервисы дефолтным ctor — доступность к фазе initialize).
    registerService<VehicleWaypointService>();
    // Лок навигации (Core-инфраструктура, без бизнеса): пока держится, /gps недоступен
    // (чекпоинт-слот занят рабочими маркерами). Держат работы (Bus/PortJobSystem) на
    // всю смену; консюмер — GpsSystem. Без зависимостей.
    registerService<NavigationLockService>();
    registerService<GridService>();
    registerService<StreamerService>();
    registerService<PlayerDialogService>();
    registerService<TextDrawService>();
    registerService<GangZoneService>();
    registerService<PickupService>();
    registerService<MapIconService>();
    registerService<CheckpointService>();
    registerService<ClassSelectionService>();
    registerService<PlayerKeyService>();
    registerService<TextLabelService>();
    registerService<WorldService>();
    registerService<PlayerActivityService>();
    registerService<ObjectEditService>();
    registerService<AudioService>();
    registerService<CameraService>();
    registerService<GameTextService>();
    // Экранные попапы через textdraw (замена неуправляемого native GameText в
    // этом сетапе, см. Docs/ScreenNotice.md). Регистрация ПОСЛЕ TextDrawService
    // и TimerService (сервис их использует через initialize в ScreenNoticeSystem).
    registerService<ScreenNoticeService>();
    // Экранный таймер обратного отсчёта через textdraw (Core, дисплей-слой). Как и
    // ScreenNoticeService, зависит только от TextDrawService (получает его через
    // initialize в ScreenTimerSystem); бизнес-система пушит остаток из своего тика.
    registerService<ScreenTimerService>();
    registerService<SpectateService>();
    registerService<MovingObjectService>();
    registerService<PlayerSkinService>();
    registerService<PlayerPersonalSkinService>();
    registerService<AttachmentService>();
    registerService<PlayerCommandService>();
    // Кулдаун репортов + журнал обращений (/mn -> «Связь с администрацией»). Без
    // зависимостей; рассылку залогиненным админам делает MenuSystem.
    registerService<ReportService>();
    registerService<AntiCheatService>();
    registerService<PlayerAnimationService>();
    registerService<PlayerChatService>();
    registerService<PlayerHealthService>();
    registerService<PlayerMoneyService>();
    registerService<PlayerSpawnService>();
    registerService<PlayerSessionService>();
    // Кэш персиста стартовой экипировки (наличные+оружие аккаунта, бизнес-фича,
    // не Core, разведена по SRP на два независимых сервиса): без зависимостей;
    // загрузку/сохранение делает PlayerMoneyPersistSystem/PlayerWeaponPersistSystem,
    // выдачу на логин-спавне — PlayerAuthSystem.
    registerService<PlayerMoneyPersistService>();
    registerService<PlayerWeaponPersistService>();
    registerService<BankService>();
    registerService<FactionService>();
    // Семьи — player-created социальные группы (бизнес-фича, не Core). Без
    // зависимостей; загрузку/команды/чат делает FamilySystem, persist —
    // write-through внутри сервиса.
    registerService<FamilyService>();
    // Дома — player houses (бизнес-фича, не Core). Источник правды о домах +
    // JSON-персист в houses.json рядом с сервером. Без зависимостей; загрузку на
    // старте, пикапы/иконки и дев-команду /house делает HouseSystem.
    registerService<HouseService>();
    // Припаркованные у дома машины (бизнес-фича, не Core): источник правды о том, какие
    // личные машины припаркованы у дома владельца (личная owner-only ИЛИ расшаренная
    // семье — режим доступа поверх той же парковки; собственность остаётся у владельца).
    // Зависит от VehicleService и FamilyService (bind в конструкторе ParkedVehicleSystem).
    // Персист в parked_vehicle (write-through); машины грузятся на старте строго после
    // семей. Логически после Family/Personal (порядок регистрации на доступность не
    // влияет — все сервисы до фазы initialize).
    registerService<ParkedVehicleService>();
    // Замок дверей личного транспорта (бизнес-фича, не Core): источник правды о том,
    // какие живые машины закрыты владельцем ПРЯМО СЕЙЧАС (сессионно, не персистится).
    // Зависит от VehicleService/PersonalVehicleService/ParkedVehicleService/FamilyService
    // (bind в конструкторе VehicleLockSystem) — регистрация после них.
    registerService<VehicleLockService>();
    // Выбор точки спавна игрока (/setspawn): источник правды о выборе (вокзал/
    // дом) + write-through в БД (player_spawn). Без зависимостей; загрузку
    // по сессии, команду/диалог и резолв точки делает SpawnChoiceSystem.
    registerService<SpawnChoiceService>();
    registerService<ElectionService>();
    // Работа-грузчик в порту (бизнес-фича, не Core): источник правды о пер-player
    // фазе цикла и балансировщике занятости 6 точек сброса. Без зависимостей;
    // пикап/чекпоинты/анимации/attach ведёт привод PortJobSystem.
    registerService<PortJobService>();
    // Персистентный кошелёк заработка в порту (write-through в БД, port_wallet):
    // сдача коробки зачисляет деньги СРАЗУ, не сгорают при дисконнекте/смерти/
    // незавершённой смене. Без зависимостей; загрузку по сессии/начисление/выдачу
    // ведёт привод PortJobSystem.
    registerService<PortWalletService>();
    // Работа-водитель автобуса (бизнес-фича, не Core): источник правды о пер-player
    // фазе смены/прогрессе маршрута, занятости 3 слотов автобусов и FIFO-очереди.
    // Без зависимостей; спавн автобусов/чекпоинты/таймер смены ведёт привод BusJobSystem.
    registerService<BusJobService>();
    // Персистентный кошелёк заработка автобусника (write-through в БД, bus_wallet):
    // зачёт чекпоинта/круга зачисляет деньги СРАЗУ, не сгорают при дисконнекте/
    // смерти/увольнении. Без зависимостей; загрузку по сессии/начисление/выдачу
    // ведёт привод BusJobSystem.
    registerService<BusWalletService>();
    // Работа-развозчик (портовый хаулер, бизнес-фича, не Core): источник правды о
    // пер-player фазе многофазного цикла (езда-туда/погрузка/езда-обратно/разгрузка),
    // занятости 4 площадок грузовиков и FIFO-очереди. Без зависимостей; спавн
    // грузовиков/чекпоинты/переноску коробок/таймер смены ведёт привод HaulerJobSystem.
    registerService<HaulerJobService>();
    // Персистентный кошелёк заработка развозчика (write-through в БД, hauler_wallet):
    // каждая разгруженная коробка и бонус за полную разгрузку зачисляются СРАЗУ, не
    // сгорают при дисконнекте/смерти/увольнении. Без зависимостей; загрузку по
    // сессии/начисление/выдачу ведёт привод HaulerJobSystem.
    registerService<HaulerWalletService>();
    // Увольнение с работы «откуда угодно» (/stopjob): чем игрок занят и как его с
    // этого уволить. Без зависимостей; каждая работа регистрирует себя в конструкторе
    // своей системы — значит, сервис обязан существовать ДО них (все сервисы
    // создаются раньше систем).
    registerService<JobDismissService>();
    // Заработок по всем работам в одном месте (/jobwallet + напоминание на входе).
    // Без зависимостей; кошельки регистрируют сами работы в конструкторах систем.
    registerService<JobWalletService>();
    // Базовая система вещей (источник правды о предметах игроков онлайн). Без
    // зависимостей; типы регистрируют системы-владельцы (MedkitSystem) в своих
    // конструкторах, persist по сессии делает InventorySystem.
    // Работа-врач: фаза смены, 2 точки спавна скорых, очередь и кулдаун лечения
    // пациентов. Без зависимостей; спавн машин/скины/деньги ведёт MedicJobSystem.
    // Каталог именованных мест мира: одна таблица «название -> координаты» для /gps,
    // /tp и выбора точки назначения в такси. Без зависимостей, наполняется в ctor.
    registerService<PlaceCatalogService>();
    // Телефон: номера игроков и вызовы служб (/c, /acceptjob). Без зависимостей;
    // службы регистрируют себя сами в конструкторах своих систем.
    registerService<PhoneService>();
    // Работа-таксист: фаза смены, 7 точек спавна, очередь и состояние поездки
    // (пассажир, назначение, депозит). Без зависимостей; машины/чекпоинты/деньги —
    // на приводе TaxiJobSystem.
    registerService<TaxiJobService>();
    registerService<MedicJobService>();
    // Персистентный кошелёк заработка врача (write-through в БД, medic_wallet).
    registerService<MedicWalletService>();
    registerService<InventoryService>();
    registerService<PlayerAuthService>();
    // Админ-доступ (источник правды об уровне/пароле/логине). Без зависимостей;
    // PlayerCommandSystem (резолвер прав) и AdminSystem берут из регистра.
    registerService<AdminService>();
    // Баны аккаунтов (источник правды в БД). Без зависимостей; AdminSystem пишет
    // (/ban), PlayerAuthSystem читает на логине.
    registerService<BanService>();
}
