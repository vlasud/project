#include "Services/ServiceRegister.h"

#include "Services/Core/AntiCheatService/AntiCheatService.h"
#include "Services/Core/GridService/GridService.h"
#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"
#include "Services/PlayerAuthService/PlayerAuthService.h"
#include "Services/Core/PlayerChatService/PlayerChatService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/PlayerVelocityService/PlayerVelocityService.h"
#include "Services/Core/PlayerWeaponService/PlayerWeaponService.h"
#include "Services/Core/StreamerService/StreamerService.h"
#include "Services/Core/TextDrawService/TextDrawService.h"
#include "Services/Core/VehicleService/VehicleService.h"

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
    registerService<TextDrawService>();
    registerService<PlayerCommandService>();
    registerService<AntiCheatService>();
    registerService<PlayerAnimationService>();
    registerService<PlayerChatService>();
    registerService<PlayerHealthService>();
    registerService<PlayerMoneyService>();
    registerService<PlayerAuthService>();
}
