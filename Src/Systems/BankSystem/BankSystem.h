#pragma once

#include "Services/BankService/BankService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Привод банка: /balance — баланс счёта аккаунта (асинхронно из БД).
// Кассы банков, снятие и переводы — будущий геймплей фракций-банков.
class BankSystem : public BaseSystem
{
  public:
    BankSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    BankService &m_bankService;
    PlayerSessionService &m_sessionService;
};
