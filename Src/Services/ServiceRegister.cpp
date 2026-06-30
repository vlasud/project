#include "Services/ServiceRegister.h"

#include "Services/AdminService/AdminService.h"
#include "Services/BanService/BanService.h"
#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/AttachmentService/AttachmentService.h"
#include "Services/BankService/BankService.h"
#include "Services/ElectionService/ElectionService.h"
#include "Services/FactionService/FactionService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/HouseService/HouseService.h"
#include "Services/InventoryService/InventoryService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
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
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/PlayerSpawnService/PlayerSpawnService.h"
#include "Services/Core/PlayerChatService/PlayerChatService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/MovingObjectService/MovingObjectService.h"
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
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
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
    // Выбор точки спавна игрока (/setspawn): источник правды о выборе (вокзал/
    // дом) + write-through в БД (player_spawn). Без зависимостей; загрузку
    // по сессии, команду/диалог и резолв точки делает SpawnChoiceSystem.
    registerService<SpawnChoiceService>();
    registerService<ElectionService>();
    // Базовая система вещей (источник правды о предметах игроков онлайн). Без
    // зависимостей; типы регистрируют системы-владельцы (MedkitSystem) в своих
    // конструкторах, persist по сессии делает InventorySystem.
    registerService<InventoryService>();
    registerService<PlayerAuthService>();
    // Админ-доступ (источник правды об уровне/пароле/логине). Без зависимостей;
    // PlayerCommandSystem (резолвер прав) и AdminSystem берут из регистра.
    registerService<AdminService>();
    // Баны аккаунтов (источник правды в БД). Без зависимостей; AdminSystem пишет
    // (/ban), PlayerAuthSystem читает на логине.
    registerService<BanService>();
}
