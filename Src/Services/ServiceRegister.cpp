#include "ServiceRegister.h"

#include "AntiCheatService/AntiCheatService.h"
#include "GridService/GridService.h"
#include "PlayerAnimationService/PlayerAnimationService.h"
#include "PlayerAuthService/PlayerAuthService.h"
#include "PlayerChatService/PlayerChatService.h"
#include "PlayerCommandService/PlayerCommandService.h"
#include "PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "PlayerDialogService/PlayerDialogService.h"
#include "PlayerHealthService/PlayerHealthService.h"
#include "PlayerLocationService/PlayerLocationService.h"
#include "PlayerStateService/PlayerStateService.h"
#include "PlayerVelocityService/PlayerVelocityService.h"
#include "PlayerWeaponService/PlayerWeaponService.h"
#include "StreamerService/StreamerService.h"
#include "VehicleService/VehicleService.h"

void ServiceRegister::registerServices()
{
    registerService<PlayerConnectionVersionService>();
    registerService<PlayerLocationService>();
    registerService<PlayerStateService>();
    registerService<PlayerVelocityService>();
    registerService<PlayerWeaponService>();
    registerService<VehicleService>();
    registerService<GridService>();
    registerService<StreamerService>();
    registerService<PlayerDialogService>();
    registerService<PlayerCommandService>();
    registerService<AntiCheatService>();
    registerService<PlayerAnimationService>();
    registerService<PlayerChatService>();
    registerService<PlayerHealthService>();
    registerService<PlayerAuthService>();
}
