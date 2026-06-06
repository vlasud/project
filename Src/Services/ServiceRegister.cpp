#include "ServiceRegister.h"

#include "PlayerAuthService/PlayerAuthService.h"

void ServiceRegister::registerServices()
{
    registerService<PlayerAuthService>();
}
