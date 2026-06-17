#include "Systems/SystemRegister.h"

#include "Systems/AdminSystem/AdminSystem.h"
#include "Systems/Core/AntiCheatSystem/AntiCheatSystem.h"
#include "Systems/Core/AttachmentSystem/AttachmentSystem.h"
#include "Systems/Core/AudioSystem/AudioSystem.h"
#include "Systems/Core/CameraSystem/CameraSystem.h"
#include "Systems/Core/ChatSystem/ChatSystem.h"
#include "Systems/Core/CheckpointSystem/CheckpointSystem.h"
#include "Systems/Core/ClassSelectionSystem/ClassSelectionSystem.h"
#include "Systems/Core/DebugCameraSystem/DebugCameraSystem.h"
#include "Systems/Core/EditorSystem/EditorSystem.h"
#include "Systems/BankSystem/BankSystem.h"
#include "Systems/PaymentSystem/PaymentSystem.h"
#include "Systems/DeathPenaltySystem/DeathPenaltySystem.h"
#include "Systems/ElectionSystem/ElectionSystem.h"
#include "Systems/FactionSystem/FactionSystem.h"
#include "Systems/Factions/ArmyAirForceSystem/ArmyAirForceSystem.h"
#include "Systems/Factions/ArmyGroundSystem/ArmyGroundSystem.h"
#include "Systems/Factions/AztecasSystem/AztecasSystem.h"
#include "Systems/Factions/BallasSystem/BallasSystem.h"
#include "Systems/Factions/BankFactionSystem/BankFactionSystem.h"
#include "Systems/Factions/FbiSystem/FbiSystem.h"
#include "Systems/Factions/GroveStreetSystem/GroveStreetSystem.h"
#include "Systems/Factions/ItalianMafiaSystem/ItalianMafiaSystem.h"
#include "Systems/Factions/PoliceLasVenturasSystem/PoliceLasVenturasSystem.h"
#include "Systems/Factions/PoliceLosSantosSystem/PoliceLosSantosSystem.h"
#include "Systems/Factions/PoliceSanFierroSystem/PoliceSanFierroSystem.h"
#include "Systems/Factions/PresidentAdministrationSystem/PresidentAdministrationSystem.h"
#include "Systems/Factions/RifaSystem/RifaSystem.h"
#include "Systems/Factions/RussianMafiaSystem/RussianMafiaSystem.h"
#include "Systems/Factions/TriadSystem/TriadSystem.h"
#include "Systems/Factions/VagosSystem/VagosSystem.h"
#include "Systems/Core/GameTextSystem/GameTextSystem.h"
#include "Systems/Core/GangZoneEditorSystem/GangZoneEditorSystem.h"
#include "Systems/Core/GangZoneSystem/GangZoneSystem.h"
#include "Systems/Core/GridDebugSystem/GridDebugSystem.h"
#include "Systems/Core/GridSystem/GridSystem.h"
#include "Systems/HelpSystem/HelpSystem.h"
#include "Systems/InventorySystem/InventorySystem.h"
#include "Systems/MedkitSystem/MedkitSystem.h"
#include "Systems/Core/LocationDebugSystem/LocationDebugSystem.h"
#include "Systems/Core/MapIconSystem/MapIconSystem.h"
#include "Systems/MenuSystem/MenuSystem.h"
#include "Systems/Core/MovingObjectSystem/MovingObjectSystem.h"
#include "Systems/Core/NicknameSystem/NicknameSystem.h"
#include "Systems/Core/ObjectEditSystem/ObjectEditSystem.h"
#include "Systems/Core/PickupSystem/PickupSystem.h"
#include "Systems/Core/PlayerActivitySystem/PlayerActivitySystem.h"
#include "Systems/Core/PlayerAnimationSystem/PlayerAnimationSystem.h"
#include "Systems/PlayerAuthSystem/PlayerAuthSystem.h"
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
#include "Systems/Core/PlayerWeaponSystem/PlayerWeaponSystem.h"
#include "Systems/Core/WeaponSkillSystem/WeaponSkillSystem.h"
#include "Systems/WeaponProficiencySystem/WeaponProficiencySystem.h"
#include "Systems/ServerLogoSystem/ServerLogoSystem.h"
#include "Systems/Core/SpectateSystem/SpectateSystem.h"
#include "Systems/Core/StreamerSystem/StreamerSystem.h"
#include "Systems/Core/TextDrawEditorSystem/TextDrawEditorSystem.h"
#include "Systems/Core/TextDrawSystem/TextDrawSystem.h"
#include "Systems/Core/TextLabelSystem/TextLabelSystem.h"
#include "Systems/Core/TimerSystem/TimerSystem.h"
#include "Systems/Core/VehicleControlSystem/VehicleControlSystem.h"
#include "Systems/Core/VehicleDebugSystem/VehicleDebugSystem.h"
#include "Systems/Core/WeaponDebugSystem/WeaponDebugSystem.h"
#include "Systems/Core/VehicleSystem/VehicleSystem.h"
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
    m_systems.push_back(std::make_unique<AudioSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<GameTextSystem>(core, serviceRegister));
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
    // GangZoneSystem раньше редактора: сервис должен получить компонент до того,
    // как тулза начнёт создавать зоны.
    m_systems.push_back(std::make_unique<GangZoneSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<GangZoneEditorSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<AntiCheatSystem>(core, serviceRegister));
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
    // SpawnSystem раньше AuthSystem: на спавне сперва применяются интерьер/мир
    // точки спавна, затем auth навешивает экипировку.
    m_systems.push_back(std::make_unique<ClassSelectionSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerSpawnSystem>(core, serviceRegister));
    // SpectateSystem после SpawnSystem: возврат из спектейта перекрывает
    // интерьер/мир точки спавна своим сохранённым местом.
    m_systems.push_back(std::make_unique<SpectateSystem>(core, serviceRegister));
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
    m_systems.push_back(std::make_unique<TriadSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<RifaSystem>(core, serviceRegister));
    // Мафии (криминал без куратора): база-интерьер с входом/выходом, как Триада/Rifa.
    m_systems.push_back(std::make_unique<RussianMafiaSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<ItalianMafiaSystem>(core, serviceRegister));
    // Банк (самостоятельная организация без куратора): база-интерьер с
    // входом/выходом, как Триада/Rifa/мафии. Не путать с BankSystem (счета игроков).
    m_systems.push_back(std::make_unique<BankFactionSystem>(core, serviceRegister));
    // Уличные банды (криминал без куратора): без базы/интерьера — спавн на турфе,
    // цвет, пул скинов и пикап-маркер территории.
    m_systems.push_back(std::make_unique<GroveStreetSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<BallasSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<VagosSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<AztecasSystem>(core, serviceRegister));
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
