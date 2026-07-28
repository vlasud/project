#include "Systems/SystemRegister.h"

#include "Systems/AdminSystem/AdminSystem.h"
#include "Systems/AutosaveSystem/AutosaveSystem.h"
#include "Systems/Core/AntiCheatSystem/AntiCheatSystem.h"
#include "Systems/Core/AntiCheatTestSystem/AntiCheatTestSystem.h"
#ifdef GAMEMODE_BENCHMARK
#include "Systems/Core/BenchmarkSystem/BenchmarkSystem.h"
#endif
#include "Systems/Core/AttachmentEditorSystem/AttachmentEditorSystem.h"
#include "Systems/Core/AttachmentSystem/AttachmentSystem.h"
#include "Systems/Core/AudioSystem/AudioSystem.h"
#include "Systems/Core/CameraSystem/CameraSystem.h"
#include "Systems/Core/ChatSystem/ChatSystem.h"
#include "Systems/Core/CheckpointSystem/CheckpointSystem.h"
#include "Systems/Core/ClassSelectionSystem/ClassSelectionSystem.h"
#include "Systems/Core/DebugCameraSystem/DebugCameraSystem.h"
#include "Systems/Core/EditorSystem/EditorSystem.h"
#include "Systems/BankSystem/BankSystem.h"
#include "Systems/BusJobSystem/BusJobSystem.h"
#include "Systems/HaulerJobSystem/HaulerJobSystem.h"
#include "Systems/JobDismissSystem/JobDismissSystem.h"
#include "Systems/JobWalletSystem/JobWalletSystem.h"
#include "Systems/MedicJobSystem/MedicJobSystem.h"
#include "Systems/PhoneSystem/PhoneSystem.h"
#include "Systems/TaxiJobSystem/TaxiJobSystem.h"
#include "Systems/CarMenuSystem/CarMenuSystem.h"
#include "Systems/PaymentSystem/PaymentSystem.h"
#include "Systems/DeathPenaltySystem/DeathPenaltySystem.h"
#include "Systems/ElectionSystem/ElectionSystem.h"
#include "Systems/FactionSystem/FactionSystem.h"
#include "Systems/FamilySystem/FamilySystem.h"
#include "Systems/GreetingSystem/GreetingSystem.h"
#include "Systems/HomeMenuSystem/HomeMenuSystem.h"
#include "Systems/HouseSystem/HouseSystem.h"
#include "Systems/ParkedVehicleSystem/ParkedVehicleSystem.h"
#include "Systems/VehicleLockSystem/VehicleLockSystem.h"
#include "Systems/SpawnChoiceSystem/SpawnChoiceSystem.h"
#include "Systems/Factions/ArmyAirForceSystem/ArmyAirForceSystem.h"
#include "Systems/Factions/ArmyGroundSystem/ArmyGroundSystem.h"
#include "Systems/Factions/BankFactionSystem/BankFactionSystem.h"
#include "Systems/Factions/FbiSystem/FbiSystem.h"
#include "Systems/Factions/PoliceLasVenturasSystem/PoliceLasVenturasSystem.h"
#include "Systems/Factions/PoliceLosSantosSystem/PoliceLosSantosSystem.h"
#include "Systems/Factions/PoliceSanFierroSystem/PoliceSanFierroSystem.h"
#include "Systems/Factions/PresidentAdministrationSystem/PresidentAdministrationSystem.h"
#include "Systems/Core/GameTextSystem/GameTextSystem.h"
#include "Systems/Core/GangZoneEditorSystem/GangZoneEditorSystem.h"
#include "Systems/Core/GangZoneSystem/GangZoneSystem.h"
#include "Systems/Core/GridDebugSystem/GridDebugSystem.h"
#include "Systems/Core/GridSystem/GridSystem.h"
#include "Systems/GpsSystem/GpsSystem.h"
#include "Systems/HelpSystem/HelpSystem.h"
#include "Systems/InventorySystem/InventorySystem.h"
#include "Systems/MedkitSystem/MedkitSystem.h"
#include "Systems/Core/LocationDebugSystem/LocationDebugSystem.h"
#include "Systems/Core/MapIconSystem/MapIconSystem.h"
#include "Systems/MenuSystem/MenuSystem.h"
#include "Systems/PersonalVehicleSystem/PersonalVehicleSystem.h"
#include "Systems/ParkingSystem/ParkingSystem.h"
#include "Systems/PortJobSystem/PortJobSystem.h"
#include "Systems/Core/MovingObjectSystem/MovingObjectSystem.h"
#include "Systems/Core/NicknameSystem/NicknameSystem.h"
#include "Systems/Core/ObjectEditSystem/ObjectEditSystem.h"
#include "Systems/Core/PickupSystem/PickupSystem.h"
#include "Systems/Core/PlayerActivitySystem/PlayerActivitySystem.h"
#include "Systems/Core/PlayerAnimationSystem/PlayerAnimationSystem.h"
#include "Systems/PlayerAuthSystem/PlayerAuthSystem.h"
#include "Systems/PlayerMoneyPersistSystem/PlayerMoneyPersistSystem.h"
#include "Systems/PlayerWeaponPersistSystem/PlayerWeaponPersistSystem.h"
#include "Systems/PlayerSessionSystem/PlayerSessionSystem.h"
#include "Systems/PlayerSpawnSystem/PlayerSpawnSystem.h"
#include "Systems/Core/PlayerCommandSystem/PlayerCommandSystem.h"
#include "Systems/Core/PlayerConnectionVersionSystem/PlayerConnectionVersionSystem.h"
#include "Systems/Core/PlayerDialogSystem/PlayerDialogSystem.h"
#include "Systems/Core/PlayerHealthSystem/PlayerHealthSystem.h"
#include "Systems/Core/PlayerKeySystem/PlayerKeySystem.h"
#include "Systems/Core/PlayerLocationSystem/PlayerLocationSystem.h"
#include "Systems/Core/PlayerMoneySystem/PlayerMoneySystem.h"
#include "Systems/Core/PlayerSkinSystem/PlayerSkinSystem.h"
#include "Systems/PlayerPersonalSkinSystem/PlayerPersonalSkinSystem.h"
#include "Systems/Core/PlayerStateSystem/PlayerStateSystem.h"
#include "Systems/Core/RoleplayChatSystem/RoleplayChatSystem.h"
#include "Systems/Core/PlayerVelocitySystem/PlayerVelocitySystem.h"
#include "Systems/Core/ScreenNoticeSystem/ScreenNoticeSystem.h"
#include "Systems/Core/ScreenTimerSystem/ScreenTimerSystem.h"
#include "Systems/Core/PlayerWeaponSystem/PlayerWeaponSystem.h"
#include "Systems/Core/WeaponSkillSystem/WeaponSkillSystem.h"
#include "Systems/WeaponProficiencySystem/WeaponProficiencySystem.h"
#include "Systems/ServerLogoSystem/ServerLogoSystem.h"
#include "Systems/SpeedometerSystem/SpeedometerSystem.h"
#include "Systems/Core/SpectateSystem/SpectateSystem.h"
#include "Systems/Core/StreamerSystem/StreamerSystem.h"
#include "Systems/Core/TextDrawEditorSystem/TextDrawEditorSystem.h"
#include "Systems/Core/TextDrawSystem/TextDrawSystem.h"
#include "Systems/Core/TextLabelSystem/TextLabelSystem.h"
#include "Systems/Core/TimerSystem/TimerSystem.h"
#include "Systems/Core/VehicleControlSystem/VehicleControlSystem.h"
#include "Systems/Core/VehicleDebugSystem/VehicleDebugSystem.h"
#include "Systems/VehicleWaypointSystem/VehicleWaypointSystem.h"
#include "Systems/Core/WeaponDebugSystem/WeaponDebugSystem.h"
#include "Systems/Core/VehicleSystem/VehicleSystem.h"
#include "Systems/VehicleNameSystem/VehicleNameSystem.h"
#include "Systems/Core/WorldSystem/WorldSystem.h"

void SystemRegister::registerSystems(ICore &core, const ServiceRegister &serviceRegister)
{
    // TimerSystem первым: его initialize() передаёт сервису компонент таймеров
    // раньше, чем другие системы смогут ставить таймеры в своих initialize().
    m_systems.push_back(std::make_unique<TimerSystem>(core, serviceRegister));
    // PlayerSessionSystem раньше всех бизнес- и core-систем: конец сессии (и
    // сохранение у подписчиков) должен отстрелить на дисконнекте ДО того, как
    // чужие обработчики начнут чистить состояние игрока.
    m_systems.push_back(std::make_unique<PlayerSessionSystem>(core, serviceRegister));
    // GreetingSystem (приветственный попап, бизнес-фича вне Core) сразу после
    // PlayerSessionSystem: подписывается на subscribeStart (сессия уже активна —
    // ник опознан). ScreenNoticeService (Core) зарегистрирован в ServiceRegister
    // заранее — доступен сразу (фактически используется только когда компоненты
    // textdraw/timer уже проинициализированы, к моменту первого логина).
    m_systems.push_back(std::make_unique<GreetingSystem>(core, serviceRegister));
    // NicknameSystem максимально рано: невалидный ник отсекается на входящем
    // подключении, до того как остальные системы заведут на игрока состояние.
    m_systems.push_back(std::make_unique<NicknameSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerConnectionVersionSystem>(core, serviceRegister));
    // LocationSystem раньше остальных: на том же апдейте все читают уже принятую
    // позицию; VelocitySystem сразу после — производная от свежей позиции.
    m_systems.push_back(std::make_unique<PlayerLocationSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerStateSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerVelocitySystem>(core, serviceRegister));
    // VehicleSystem раньше GridSystem: на смене стейта привязка пассажира уже
    // сделана, грид читает машину игрока из источника правды.
    m_systems.push_back(std::make_unique<VehicleSystem>(core, serviceRegister));
    // VehicleNameSystem (попап названия машины при посадке за руль, бизнес-фича
    // вне Core) сразу после VehicleSystem: bindOccupant уже проставил водителя
    // на смене стейта, getVehicle(playerId) видит машину. ScreenNoticeService
    // (Core) зарегистрирован в ServiceRegister заранее — доступен сразу (фактически
    // используется только во время игры, когда все системы уже сконструированы).
    m_systems.push_back(std::make_unique<VehicleNameSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<GridSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<StreamerSystem>(core, serviceRegister));
    // PickupSystem после StreamerSystem: маршрутизация подбора опирается на
    // пикапы, созданные стримером.
    m_systems.push_back(std::make_unique<PickupSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<TextLabelSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<WorldSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerActivitySystem>(core, serviceRegister));
    // ObjectEditSystem раньше EditorSystem: роутер событий редактирования должен
    // существовать до того, как редактор начнёт сессии.
    m_systems.push_back(std::make_unique<ObjectEditSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<MovingObjectSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerSkinSystem>(core, serviceRegister));
    // Личный скин аккаунта: лайфцикл слота + персист на конце сессии. После
    // PlayerSessionSystem (подписка на конец сессии). Загрузку/применение на
    // входе делает PlayerAuthSystem (у него логин-select).
    m_systems.push_back(std::make_unique<PlayerPersonalSkinSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<AttachmentSystem>(core, serviceRegister));
    // AttachmentEditorSystem (дев-тулинг /aedit) сразу после AttachmentSystem: тот
    // подключает обработчик клиентской подгонки (гизмо) объектов к диспетчеру,
    // которым пользуется команда редактора.
    m_systems.push_back(std::make_unique<AttachmentEditorSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<AudioSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<GameTextSystem>(core, serviceRegister));
    // ScreenNoticeSystem (экранные попапы через textdraw, замена неуправляемого
    // native GameText в этом сетапе) — держит зависимость на TextDrawService и
    // TimerService, оба зарегистрированы в ServiceRegister заранее. Компонент
    // textdraw сервис получает через TextDrawSystem::initialize() позже (фаза
    // initializeSystems идёт отдельным проходом после конструирования всех
    // систем) — на порядок конструирования это не влияет.
    m_systems.push_back(std::make_unique<ScreenNoticeSystem>(core, serviceRegister));
    // ScreenTimerSystem (экранный таймер-бар через textdraw, дисплей-слой) рядом с
    // ScreenNoticeSystem: держит зависимость на TextDrawService (зарегистрирован в
    // ServiceRegister заранее), компонент textdraw сервис получает через
    // TextDrawSystem::initialize() позже — на порядок конструирования не влияет.
    m_systems.push_back(std::make_unique<ScreenTimerSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<MapIconSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<CheckpointSystem>(core, serviceRegister));
    // m_systems.push_back(std::make_unique<GridDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<LocationDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<VehicleDebugSystem>(core, serviceRegister));
    // WeaponDebugSystem раньше PlayerWeaponSystem: замер темпа (/rof) видит сырой
    // клиентский поток выстрелов до того, как валидатор начнёт дропать.
    m_systems.push_back(std::make_unique<WeaponDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerDialogSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerCommandSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerKeySystem>(core, serviceRegister));
    // VehicleControlSystem после PlayerKeySystem и VehicleSystem: на момент работы
    // оба сервиса уже привязаны (VehicleService::bind в initialize VehicleSystem),
    // а клавишный поток маршрутизируется PlayerKeySystem.
    m_systems.push_back(std::make_unique<VehicleControlSystem>(core, serviceRegister));
    // TextDrawSystem раньше редактора: роутер кликов должен существовать до того,
    // как редактор начнёт регистрировать обработчики.
    m_systems.push_back(std::make_unique<TextDrawSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<TextDrawEditorSystem>(core, serviceRegister));
    // ServerLogoSystem после TextDrawSystem: к моменту его initialize() сервис уже
    // получил компонент textdraw, и логотип создаётся успешно.
    m_systems.push_back(std::make_unique<ServerLogoSystem>(core, serviceRegister));
    // SpeedometerSystem после TextDrawSystem (зависит от компонента textdraw — к
    // его initialize() сервис уже получил компонент; иначе HUD отключится).
    // VehicleService/PlayerVelocityService/TimerService зарегистрированы раньше.
    m_systems.push_back(std::make_unique<SpeedometerSystem>(core, serviceRegister));
    // VehicleWaypointSystem (привод указателя-на-машину, бизнес-фича вне Core) до
    // ParkingSystem и CarMenuSystem (они ставят указатель через VehicleWaypointService).
    // В конструкторе связывает сервис с CheckpointService (bind) и подписывается на
    // уничтожение машин (снять указатель на пропавшую цель). VehicleService/
    // CheckpointService/VehicleWaypointService зарегистрированы раньше. Указатель —
    // холодный путь (по подаче/команде/уничтожению), не per-tick.
    m_systems.push_back(std::make_unique<VehicleWaypointSystem>(core, serviceRegister));
    // PersonalVehicleSystem (личный транспорт, бизнес-фича вне Core) после
    // VehicleSystem (VehicleService уже связан с пулом машин), PlayerCommandSystem
    // (резолвер прав /pvbuy) и PlayerSessionSystem (жизненный цикл владения — по
    // старту/концу сессии). В конструкторе связывает PersonalVehicleService с
    // VehicleService (bind), подписывается на старт/конец сессии (загрузка владения
    // из БД с serial-guard / reset) и уничтожение машин, регистрирует дебаг-команду
    // /pvbuy. Загрузка владения — один async-select на старте сессии, не per-tick.
    m_systems.push_back(std::make_unique<PersonalVehicleSystem>(core, serviceRegister));
    // ParkingSystem (парковка личного транспорта, бизнес-фича вне Core) после
    // PersonalVehicleSystem (PersonalVehicleService уже связан с VehicleService через
    // bind), VehicleSystem (VehicleService::anyVehicleNear), и core-систем пикапов/
    // чекпоинтов/диалога/3D-текста (PickupSystem/CheckpointSystem/PlayerDialogSystem/
    // TextLabelSystem зарегистрированы раньше — к initialize() ParkingSystem их сервисы
    // уже получили компоненты, и пикап/лейбл парковки создаются успешно). Спавн машины
    // на свободной точке + чекпоинт — холодный путь (по пикапу/диалогу), не per-tick.
    m_systems.push_back(std::make_unique<ParkingSystem>(core, serviceRegister));
    // GangZoneSystem раньше редактора: сервис должен получить компонент до того,
    // как тулза начнёт создавать зоны.
    m_systems.push_back(std::make_unique<GangZoneSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<GangZoneEditorSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<AntiCheatSystem>(core, serviceRegister));
    // Дев-панель проверки детекторов (/actest): только читает сервисы и регистрирует
    // команду, на порядок работы анти-чита не влияет.
    m_systems.push_back(std::make_unique<AntiCheatTestSystem>(core, serviceRegister));
#ifdef GAMEMODE_BENCHMARK
    // Дев-бенчмарк (/bench): только читает сервисы и меряет время, на игровую
    // логику не влияет.
    m_systems.push_back(std::make_unique<BenchmarkSystem>(core, serviceRegister));
#endif
    m_systems.push_back(std::make_unique<PlayerAnimationSystem>(core, serviceRegister));
    // WeaponSystem раньше HealthSystem: фейковый выстрел (оружие без выдачи)
    // отбрасывается до регистрации bullet sync в health — не легализует give-damage.
    m_systems.push_back(std::make_unique<PlayerWeaponSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<WeaponSkillSystem>(core, serviceRegister));
    // Прогрессия владения оружием: лайфцикл слота + persist по сессии (после
    // PlayerSessionSystem — подписка на старт/конец сессии). Сам инкремент делает
    // PlayerWeaponSystem на валидном выстреле; здесь только загрузка/сохранение.
    m_systems.push_back(std::make_unique<WeaponProficiencySystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerHealthSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerMoneySystem>(core, serviceRegister));
    // PortJobSystem (работа-грузчик в порту, бизнес-фича вне Core) после PickupSystem
    // (initialize зовёт PickupService::add — компонент пикапов уже подключён),
    // CheckpointSystem, PlayerAnimationSystem, AttachmentSystem, PlayerMoneySystem
    // (выдача наличных на «Забрать деньги» — заработок за сдачу копится персистентно
    // в PortWalletService, write-through), PlayerLocationSystem и PlayerSessionSystem
    // (подписка на старт сессии — загрузка кошелька порта; на конец — сброс
    // волатильного состояния смены и памяти кошелька, БД не трогает). Цикл целиком
    // событийный (пикап/чекпоинт/таймер), per-tick работы нет.
    m_systems.push_back(std::make_unique<PortJobSystem>(core, serviceRegister));
    // BusJobSystem (работа-водитель автобуса, бизнес-фича вне Core) после
    // TimerSystem (общий per-second таймер смены в initialize), VehicleSystem
    // (VehicleService связан с пулом — спавн автобусов Owner::Work + driver-gate),
    // PickupSystem/MapIconSystem/CheckpointSystem (пикап/иконка/чекпоинты маршрута
    // в initialize — компоненты уже подключены), PlayerMoneySystem (выдача наличных
    // на «Забрать деньги» — заработок копится в BusWalletService, write-through),
    // PlayerLocationSystem (зона stop-чекпоинта по принятой позиции), PlayerHealthSystem
    // (subscribeDeath — увольнение по смерти) и PlayerSessionSystem (старт — загрузка
    // кошелька; конец — teardown смены + сброс кэша). Геймплей событийный + один
    // общий per-second таймер, обходящий снимок 3 слотов (не per-tick).
    m_systems.push_back(std::make_unique<BusJobSystem>(core, serviceRegister));
    // HaulerJobSystem (работа-развозчик, бизнес-фича вне Core) после тех же систем,
    // что и BusJobSystem (TimerSystem — общий per-second таймер депо; VehicleSystem —
    // спавн грузовиков Owner::Work + driver-gate; Pickup/MapIcon/CheckpointSystem —
    // пикап/иконка/чекпоинты; PlayerMoneySystem — выдача наличных; PlayerLocation/
    // PlayerState — серверная позиция/стейт; PlayerHealthSystem — увольнение по
    // смерти; PlayerSessionSystem — кошелёк по сессии), плюс PlayerAnimationSystem/
    // AttachmentSystem (переноска коробки) и PortJobSystem (единый источник координат
    // склада — PortJobService::dropPositions, статический, регистрация выше). Депо-
    // часть O(SLOT_COUNT) в общем per-second таймере, остальное событийно.
    m_systems.push_back(std::make_unique<HaulerJobSystem>(core, serviceRegister));
    // JobDismissSystem (/stopjob) — ПОСЛЕ всех работ: список наполняют их
    // конструкторы, а команда резолвит работу на момент вызова. Порядок здесь не
    // критичен (регистрации и вызов разнесены во времени), но так видно зависимость.
    // MedicJobSystem (работа-врач, бизнес-фича вне Core) после тех же систем, что и
    // прочие работы: TimerSystem (общий per-second таймер больницы), VehicleSystem
    // (спавн скорых Owner::Work + driver-gate), Pickup/MapIconSystem (пикап и иконка
    // в initialize), PlayerMoneySystem (выдача наличных), PlayerLocation/PlayerState
    // (серверные позиция и стейт для /med), PlayerHealthSystem (лечение + увольнение
    // по смерти), PlayerSkinSystem (форменный скин) и PlayerSessionSystem (кошелёк и
    // пол аккаунта по сессии). Маршрута нет — только окно выезда в общем таймере.
    m_systems.push_back(std::make_unique<MedicJobSystem>(core, serviceRegister));
    // TaxiJobSystem (работа-таксист, бизнес-фича вне Core) — те же зависимости, что и
    // прочие работы, плюс CheckpointSystem (чекпоинт точки назначения) и клик по карте
    // (подписка на PlayerClickDispatcher в конструкторе). Кошелька нет: деньги за
    // поездку идут от пассажира и отдаются на руки по прибытии.
    m_systems.push_back(std::make_unique<TaxiJobSystem>(core, serviceRegister));
    // PhoneSystem — ПОСЛЕ работ (они кладут в телефон свои службы) и после
    // PlayerSessionSystem/FactionSystem: номер грузится по старту сессии, а полицию
    // телефон перечисляет через членство во фракции.
    m_systems.push_back(std::make_unique<PhoneSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<JobDismissSystem>(core, serviceRegister));
    // JobWalletSystem (/jobwallet) — тоже после работ: кошельки в список кладут их
    // конструкторы. Напоминание о деньгах шлётся по сигналу загрузки кошелька, а не
    // на старте сессии (загрузка асинхронная), поэтому порядок подписок не важен.
    m_systems.push_back(std::make_unique<JobWalletSystem>(core, serviceRegister));
    // SpawnSystem раньше AuthSystem: на спавне сперва применяются интерьер/мир
    // точки спавна, затем auth навешивает экипировку.
    m_systems.push_back(std::make_unique<ClassSelectionSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerSpawnSystem>(core, serviceRegister));
    // SpectateSystem после SpawnSystem: возврат из спектейта перекрывает
    // интерьер/мир точки спавна своим сохранённым местом.
    m_systems.push_back(std::make_unique<SpectateSystem>(core, serviceRegister));
    // Персист стартовой экипировки (наличные+оружие аккаунта, бизнес-фича, не
    // Core, разведена по SRP на два независимых сервиса+системы) до
    // PlayerAuthSystem: тот на логин-спавне читает кэш PlayerMoneyPersistService/
    // PlayerWeaponPersistService (и подписывается на late-загрузку) — сервисы
    // уже должны принимать подписки к этому моменту. Порядок конструирования
    // здесь не критичен (обе подписки регистрируются в конструкторах ДО первого
    // реального события), но так нагляднее видна зависимость.
    m_systems.push_back(std::make_unique<PlayerMoneyPersistSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerWeaponPersistSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerAuthSystem>(core, serviceRegister));
    // FactionSystem после auth: подписки на сессию (членство грузится по её
    // старту). Конкретные фракции регистрируются после неё.
    m_systems.push_back(std::make_unique<FactionSystem>(core, serviceRegister));
    // Конкретные фракции: регистрируют себя и свои базы в конструкторах —
    // до FactionSystem::initialize (он грузит ранги и создаёт пикапы баз).
    m_systems.push_back(std::make_unique<PresidentAdministrationSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PoliceLosSantosSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PoliceSanFierroSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PoliceLasVenturasSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<FbiSystem>(core, serviceRegister));
    // Банк (самостоятельная организация без куратора): база-интерьер с входом/выходом.
    // Не путать с BankSystem (счета игроков).
    m_systems.push_back(std::make_unique<BankFactionSystem>(core, serviceRegister));
    // Армия (госструктуры, куратор — Администрация Президента, как полиция/ФБР):
    // без базы/интерьера — спавн на территории базы, цвет, пул скинов и
    // пикап-маркер базы.
    m_systems.push_back(std::make_unique<ArmyGroundSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<ArmyAirForceSystem>(core, serviceRegister));
    // Админ-система после auth/session/faction: персист уровня по старту сессии
    // (serial-guard), резолвер прав команд берёт уровень из AdminService.
    m_systems.push_back(std::make_unique<AdminSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<BankSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PaymentSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<ElectionSystem>(core, serviceRegister));
    // DeathPenaltySystem ПОСЛЕ PlayerHealthSystem (его onPlayerSpawn -> HP=100
    // должен отработать раньше, чтобы наш setMaxHealth зажал уже выставленное HP) и
    // ПОСЛЕ PlayerSessionSystem (штраф привязан к аккаунту — сессия должна быть
    // доступна). Бизнес-фича — вне Core.
    m_systems.push_back(std::make_unique<DeathPenaltySystem>(core, serviceRegister));
    // Вещи (базовая система предметов): персист по сессии (после PlayerSessionSystem
    // — подписка на старт/конец сессии) + дев-выдача /idev. Бизнес-фича, вне Core.
    m_systems.push_back(std::make_unique<InventorySystem>(core, serviceRegister));
    // Аптечка (первый предмет поверх вещей): регистрирует свой тип в InventoryService
    // в конструкторе и добавляет /healme. После InventorySystem логически
    // (порядок реестра типов от порядка систем не зависит — сервис уже сконструирован).
    m_systems.push_back(std::make_unique<MedkitSystem>(core, serviceRegister));
    // Семьи (player-created соц-группы, бизнес-фича вне Core): после
    // PlayerSessionSystem (подписки на старт/конец сессии резолвят онлайн-членство).
    // Сервисы команд/диалога/чата зарегистрированы раньше (Core). Источник правды
    // о семьях — в БД (write-through), грузится на initialize.
    m_systems.push_back(std::make_unique<FamilySystem>(core, serviceRegister));
    // Дома (player houses, бизнес-фича вне Core): дев-команда /house в
    // конструкторе, загрузка houses.json на initialize. Core-сервисы команд/
    // диалога/пикапов/иконок/локации зарегистрированы раньше; их системы
    // (PickupSystem/MapIconSystem/StreamerSystem/PlayerLocationSystem/
    // PlayerDialogSystem) тоже инициализируются раньше — к моменту, когда придёт
    // async-колбэк загрузки и начнёт заводить пикапы/иконки, всё уже подключено.
    m_systems.push_back(std::make_unique<HouseSystem>(core, serviceRegister));
    // ParkedVehicleSystem (припаркованные у дома машины, бизнес-фича вне Core) после
    // FamilySystem (загрузка parked_vehicle идёт через FamilyService::subscribeLoaded —
    // строго после семей: они нужны для гейта РАСШАРЕННЫХ; обе загрузки async, порядком
    // регистрации гарантию не дать), ParkingSystem и CarMenuSystem (они лишь берут
    // ParkedVehicleService из реестра — сам сервис сконструирован в ServiceRegister). В
    // конструкторе связывает ParkedVehicleService с VehicleService/FamilyService (bind),
    // подписывается на driver-gate (чужой не за руль) и уничтожение машин. VehicleSystem
    // (VehicleService связан с пулом) зарегистрирован намного раньше — к async-колбэку
    // загрузки create спавнит машины у домов успешно. Спавн на старте + гейт O(1),
    // не per-tick.
    m_systems.push_back(std::make_unique<ParkedVehicleSystem>(core, serviceRegister));
    // VehicleLockSystem (замок дверей личного транспорта, бизнес-фича вне Core) сразу
    // после ParkedVehicleSystem: bind дёргает ParkedVehicleService/PersonalVehicleService/
    // FamilyService/VehicleService (все зарегистрированы/сконструированы раньше) и
    // подписывается на ParkedVehicleService::subscribeReconcile (share/unshare меняет
    // состав «свой» уже закрытой машины). ДО CarMenuSystem — он открывает/закрывает
    // замок через VehicleLockService::toggle. Событийная, per-tick работы нет.
    m_systems.push_back(std::make_unique<VehicleLockSystem>(core, serviceRegister));
    // CarMenuSystem (/car — меню личного транспорта, бизнес-фича вне Core) после
    // VehicleWaypointSystem (VehicleWaypointService связан с CheckpointService),
    // PersonalVehicleSystem (владение резолвится), ParkedVehicleSystem (размещение/
    // респавн у дома) и VehicleLockSystem (замок дверей «Текущая машина»). Core-системы
    // команд/диалога зарегистрированы раньше. Команда /car регистрируется в
    // конструкторе; меню/под-диалоги/указатель/замок — холодный путь.
    m_systems.push_back(std::make_unique<CarMenuSystem>(core, serviceRegister));
    // HomeMenuSystem (/home — меню владельца дома, бизнес-фича вне Core) после
    // HouseSystem (владение/иконки/персист владения через subscribeOwnerChanged —
    // подписчик там, HomeMenuSystem лишь зовёт HouseService::setOwner),
    // ParkedVehicleSystem (unpark припаркованных при передаче/выселении) и
    // VehicleWaypointSystem (общий чекпоинт-указатель, target-режим «точка» для
    // входа дома). Команда /home регистрируется в конструкторе; меню — холодный путь.
    m_systems.push_back(std::make_unique<HomeMenuSystem>(core, serviceRegister));
    // SpawnChoiceSystem (/setspawn, бизнес-фича вне Core) после: PlayerSessionSystem
    // (подписки на старт/конец сессии), PlayerSpawnSystem (спавн-сервис),
    // FactionSystem + конкретные фракции (спавны орг уже зарегистрированы) и
    // HouseSystem (владение домами грузится на старте — резолв «Дом» видит дома).
    // Core-сервисы команд/диалога зарегистрированы раньше. Загрузка выбора — по
    // старту сессии (serial-guard), применение/резолв — по входу/выбору, не per-tick.
    m_systems.push_back(std::make_unique<SpawnChoiceSystem>(core, serviceRegister));
    // GpsSystem (/gps всем + /tp админам, бизнес-фича вне Core) после VehicleWaypointSystem
    // (VehicleWaypointService связан с CheckpointService — единый чекпоинт-слот GPS),
    // Bus/PortJobSystem (держат NavigationLockService — GpsSystem подписывается на его
    // acquire для гашения GPS; подписка в конструкторе успевает до первого устройства
    // на работу в игре), HouseSystem/FactionSystem (динамические места «Мой дом»/«Моя
    // работа» резолвятся от них в момент клика). Core-сервисы команд/диалога/локации/
    // сессии зарегистрированы раньше. Команды в конструкторе; диалог/чекпоинт/телепорт —
    // холодный путь (по команде), не per-tick.
    m_systems.push_back(std::make_unique<GpsSystem>(core, serviceRegister));
    // AutosaveSystem после всех save-подписчиков (Inventory/WeaponProficiency/
    // PersonalSkin) и PlayerSessionSystem: к его initialize() (где ставится таймер)
    // все персистеры уже подписались в своих конструкторах. Периодический автосейв
    // онлайн-игроков (open.mp не шлёт onPlayerDisconnect на штатной остановке).
    m_systems.push_back(std::make_unique<AutosaveSystem>(core, serviceRegister));
    // CameraSystem раньше тулзы: проигрыватель путей должен быть подключён до
    // того, как /camera начнёт им пользоваться.
    m_systems.push_back(std::make_unique<CameraSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<DebugCameraSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<EditorSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<ChatSystemSystem>(core, serviceRegister));
    // RoleplayChatSystem после ChatSystem: те же сервисы (команды/грид/локация/чат),
    // общий с чатом барьер мута/антиспама через PlayerChatService.
    m_systems.push_back(std::make_unique<RoleplayChatSystem>(core, serviceRegister));
    // HelpSystem последней: строит /help из реестра команд — все команды уже
    // зарегистрированы конструкторами выше (порядок для неё некритичен).
    m_systems.push_back(std::make_unique<HelpSystem>(core, serviceRegister));
    // MenuSystem (/mn) — холодный путь (по команде/диалогу); порядок некритичен.
    // Пункт «Помощь» зовёт тот же help-слой; «Связь с администрацией» рассылает
    // репорт залогиненным админам и пишет в журнал ReportService.
    m_systems.push_back(std::make_unique<MenuSystem>(core, serviceRegister));
}

void SystemRegister::initializeSystems(IComponentList *components)
{
    for (const auto &system : m_systems)
    {
        system->initialize(components);
    }
}

void SystemRegister::resetSystems()
{
    for (const auto &system : m_systems)
    {
        system->reset();
    }
}
