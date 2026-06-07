#include "ServiceRegister.h"

#include "PlayerAuthService/PlayerAuthService.h"
#include "PlayerConnectionVersionService/PlayerConnectionVersionService.h"
#include "PlayerDialogService/PlayerDialogService.h"

void ServiceRegister::registerServices()
{
    registerService<PlayerConnectionVersionService>();
    registerService<PlayerDialogService>();
    registerService<PlayerAuthService>();
}
