#include "Services/ServiceRegister.h"

#include "Services/Core/AntiCheatService/AntiCheatService.h"
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
#include "Services/PlayerSpawnService/PlayerSpawnService.h"
#include "Services/Core/PlayerChatService/PlayerChatService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/ObjectEditService/ObjectEditService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerKeyService/PlayerKeyService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/PlayerVelocityService/PlayerVelocityService.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
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
    registerService<PlayerConnectionVersionService>();
    registerService<PlayerLocationService>();
    registerService<PlayerStateService>();
    registerService<PlayerVelocityService>();
    registerService<PlayerWeaponService>();
    registerService<VehicleService>();
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
    registerService<PlayerCommandService>();
    registerService<AntiCheatService>();
    registerService<PlayerAnimationService>();
    registerService<PlayerChatService>();
    registerService<PlayerHealthService>();
    registerService<PlayerMoneyService>();
    registerService<PlayerSpawnService>();
    registerService<PlayerAuthService>();
}
