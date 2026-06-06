#include "ServiceRegister.h"

#include "PlayerAuthService/PlayerAuthService.h"
#include "PlayerConnectionVersionService/PlayerConnectionVersionService.h"

void ServiceRegister::registerServices()
{
    registerService<PlayerConnectionVersionService>();
    registerService<PlayerAuthService>();
}
