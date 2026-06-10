#include "ServiceRegister.h"

#include "AntiCheatService/AntiCheatService.h"
#include "PlayerAnimationService/PlayerAnimationService.h"
#include "PlayerAuthService/PlayerAuthService.h"
#include "PlayerCommandService/PlayerCommandService.h"
#include "PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "PlayerDialogService/PlayerDialogService.h"

void ServiceRegister::registerServices()
{
    registerService<PlayerConnectionVersionService>();
    registerService<PlayerDialogService>();
    registerService<PlayerCommandService>();
    registerService<AntiCheatService>();
    registerService<PlayerAnimationService>();
    registerService<PlayerAuthService>();
}
